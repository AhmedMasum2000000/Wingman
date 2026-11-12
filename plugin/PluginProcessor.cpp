#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace backline;

namespace
{
constexpr double maxCaptureSeconds = 120.0;

struct SimpleSound : juce::SynthesiserSound
{
    bool appliesToNote (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

// Two detuned saws (chords) or sine plus a little saw (bass) through a one-pole low-pass.
class SimpleVoice : public juce::SynthesiserVoice
{
public:
    explicit SimpleVoice (bool isBass) : bass (isBass) {}

    bool canPlaySound (juce::SynthesiserSound*) override { return true; }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        const double sr = getSampleRate();
        const double freq = juce::MidiMessage::getMidiNoteInHertz (midiNote);
        inc1 = freq / sr;
        inc2 = freq * 1.006 / sr;
        phase1 = 0.0; phase2 = 0.3;
        level = velocity * (bass ? 0.35f : 0.12f);
        coeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * (bass ? 500.0f : 2200.0f) / (float) sr);
        filterState = 0.0f;
        adsr.setSampleRate (sr);
        adsr.setParameters ({ 0.005f, bass ? 0.2f : 0.4f, bass ? 0.8f : 0.6f, bass ? 0.08f : 0.25f });
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff) { adsr.noteOff(); return; }
        adsr.reset();
        clearCurrentNote();
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! isVoiceActive())
            return;
        for (int i = 0; i < numSamples; ++i)
        {
            const float saw1 = (float) (2.0 * phase1 - 1.0), saw2 = (float) (2.0 * phase2 - 1.0);
            const float raw = bass ? std::sin (juce::MathConstants<float>::twoPi * (float) phase1) + 0.3f * saw1
                                   : 0.5f * (saw1 + saw2);
            filterState += coeff * (raw - filterState);
            const float sample = filterState * level * adsr.getNextSample();
            for (int ch = 0; ch < output.getNumChannels(); ++ch)
                output.addSample (ch, startSample + i, sample);
            if ((phase1 += inc1) >= 1.0) phase1 -= 1.0;
            if ((phase2 += inc2) >= 1.0) phase2 -= 1.0;
            if (! adsr.isActive()) { clearCurrentNote(); break; }
        }
    }

private:
    const bool bass;
    double phase1 = 0, phase2 = 0, inc1 = 0, inc2 = 0;
    float level = 0, coeff = 0, filterState = 0;
    juce::ADSR adsr;
};

double positiveMod (double x, double m) { const double r = std::fmod (x, m); return r < 0 ? r + m : r; }
}

BacklineProcessor::BacklineProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (int i = 0; i < 12; ++i) chordSynth.addVoice (new SimpleVoice (false));
    for (int i = 0; i < 4; ++i)  bassSynth.addVoice (new SimpleVoice (true));
    chordSynth.addSound (new SimpleSound());
    bassSynth.addSound (new SimpleSound());
    startTimerHz (10);
}

BacklineProcessor::~BacklineProcessor() { stopTimer(); }

void BacklineProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    // Hosts call this often (some on every transport start) and with changing sample rates.
    // The take buffer is only allocated here the first time; an existing take is never touched.
    if (capture.empty())
        capture.assign ((size_t) (sampleRate * maxCaptureSeconds), 0.0f);
    chordSynth.setCurrentPlaybackSampleRate (sampleRate);
    bassSynth.setCurrentPlaybackSampleRate (sampleRate);
    synthBuffer.setSize (2, samplesPerBlock);
    chordMidi.ensureSize (4096);
    bassMidi.ensureSize (4096);
    haltPreviewAndTake();
}

bool BacklineProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && layouts.getMainInputChannelSet() == out;
}

// Stops everything that depends on blocks arriving continuously (bypass, suspend, re-prepare).
void BacklineProcessor::haltPreviewAndTake()
{
    chordSynth.allNotesOff (0, false);
    bassSynth.allNotesOff (0, false);
    wasPlaying = false;
    nextBeat = -1.0;
    playheadBeats = -1.0;
    if (recording.exchange (false))
    {
        recordArmed = false;
        analysisPending = true;
    }
}

void BacklineProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    haltPreviewAndTake();
    juce::AudioProcessor::processBlockBypassed (buffer, midi);
}

// Sends the note events that fall in the block [blockStart, blockEnd) of a loop. Consecutive
// blocks must pass the previous blockEnd as their blockStart, so every event is sent exactly once
// even when it lands precisely on a block boundary.
void BacklineProcessor::scheduleNotes (const std::vector<Note>& notes, juce::MidiBuffer& midi, double blockStart,
                                       double blockEnd, double loopBeats, int numSamples)
{
    if (blockStart == blockEnd || loopBeats <= 0.0 || numSamples <= 0)
        return;
    const double span = positiveMod (blockEnd - blockStart, loopBeats);
    auto offsetFor = [&] (double beat, int& offset)
    {
        const bool inside = blockStart < blockEnd ? (beat >= blockStart && beat < blockEnd)
                                                  : (beat >= blockStart || beat < blockEnd);
        if (inside)
            offset = juce::jlimit (0, numSamples - 1, (int) (positiveMod (beat - blockStart, loopBeats) / span * numSamples));
        return inside;
    };

    int offset = 0;
    for (const auto& n : notes) // note-offs first so repeated pitches retrigger cleanly
        if (offsetFor (std::min (n.start + n.length, loopBeats - 0.001), offset))
            midi.addEvent (juce::MidiMessage::noteOff (1, n.pitch), offset);
    for (const auto& n : notes)
        if (n.start < loopBeats && offsetFor (n.start, offset))
            midi.addEvent (juce::MidiMessage::noteOn (1, n.pitch, (juce::uint8) n.velocity), offset);
}

void BacklineProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples(), numChannels = buffer.getNumChannels();
    for (int ch = getTotalNumInputChannels(); ch < numChannels; ++ch)
        buffer.clear (ch, 0, numSamples);
    if (numSamples <= 0)
        return;
    const double sampleRate = currentSampleRate.load();

    // Transport: the host's when it provides musical time, otherwise a free-running clock.
    // (The standalone app and some simple hosts supply a playhead with no tempo or position.)
    double hostBpm = 120.0, ppq = internalPpq, beatsPerBar = 4.0, barStart = 0.0;
    bool playing = true, hasBarStart = false, hostTransport = false;
    if (auto* hostPlayHead = getPlayHead())
        if (auto position = hostPlayHead->getPosition())
            if (position->getPpqPosition().hasValue() || position->getBpm().hasValue())
            {
                hostTransport = true;
                playing = position->getIsPlaying();
                if (auto b = position->getBpm()) hostBpm = juce::jlimit (20.0, 999.0, *b);
                if (auto p = position->getPpqPosition()) ppq = *p;
                if (auto sig = position->getTimeSignature())
                    beatsPerBar = juce::jmax (1.0, sig->numerator * 4.0 / juce::jmax (1, sig->denominator));
                if (auto b = position->getPpqPositionOfLastBarStart()) { barStart = *b; hasBarStart = true; }
            }
    const double blockBeats = numSamples * hostBpm / (60.0 * sampleRate);
    internalPpq += blockBeats;

    // Capture the incoming audio (mono mix) while armed and the transport is running.
    if (recording.load() || recordArmed.load())
    {
        auto startTake = [&]
        {
            // Recording may start mid-bar; remember how far it is to the next bar line.
            const double intoBar = positiveMod (hasBarStart ? ppq - barStart : ppq, beatsPerBar);
            captureSkipBeats = (! hostTransport || intoBar < 0.05) ? 0.0 : beatsPerBar - intoBar;
            capturedSamples = 0;
            captureStartPpq = ppq;
            captureBpm = hostBpm;
            captureBeatsPerBar = beatsPerBar;
            captureSampleRate = sampleRate;
            expectedRecordPpq = ppq;
            recording = true;
        };

        if (! recording.load() && playing && ! capture.empty())
            startTake();

        if (recording.load())
        {
            bool finish = ! recordArmed.load() || ! playing;

            // The take must map linearly onto the timeline: a loop wrap, a seek or a tempo change ends it
            // (or restarts it, if there is not yet a bar's worth of audio).
            const bool jumped = hostTransport && (std::abs (ppq - expectedRecordPpq) > std::max (0.01, 0.5 * blockBeats)
                                                  || std::abs (hostBpm - captureBpm.load()) > 0.01);
            if (! finish && jumped)
            {
                const double beatsSoFar = capturedSamples.load() / sampleRate * captureBpm.load() / 60.0 - captureSkipBeats.load();
                if (beatsSoFar >= captureBeatsPerBar.load())
                    finish = true;
                else
                    startTake();
            }

            if (! finish)
            {
                const int count = capturedSamples.load();
                const int toCopy = juce::jmin (numSamples, (int) capture.size() - count);
                const int inputs = juce::jmin (getTotalNumInputChannels(), numChannels);
                for (int i = 0; i < toCopy; ++i)
                {
                    float mix = 0.0f;
                    for (int ch = 0; ch < inputs; ++ch)
                        mix += buffer.getSample (ch, i);
                    capture[(size_t) (count + i)] = mix / (float) juce::jmax (1, inputs);
                }
                capturedSamples = count + toCopy;
                expectedRecordPpq = ppq + blockBeats;
                finish = toCopy < numSamples; // buffer full
            }

            if (finish)
            {
                recordArmed = false;
                recording = false;
                analysisPending = true;
            }
        }
    }

    // Pick up a newly published progression. The old one is handed back for the message thread to free.
    {
        const juce::SpinLock::ScopedTryLockType lock (playbackLock);
        if (lock.isLocked() && activePlayback != pendingPlayback && releasedPlayback == nullptr)
        {
            releasedPlayback = std::move (activePlayback);
            activePlayback = pendingPlayback;
            nextBeat = -1.0; // restart the notes from the current position
        }
    }

    // Preview the generated chords and bass in time with the transport.
    chordMidi.clear();
    bassMidi.clear();
    const bool hasLoop = playing && activePlayback != nullptr && activePlayback->lengthBeats > 0.0;
    const double loop = hasLoop ? activePlayback->lengthBeats : 1.0;
    const double startBeat = hasLoop ? positiveMod (ppq - activePlayback->originPpq, loop) : -1.0;
    playheadBeats = startBeat;
    const bool shouldPlay = hasLoop && previewEnabled.load();
    if (shouldPlay)
    {
        // Continuous playback carries on from where the last block ended. Anything else (start, seek,
        // host loop wrap, edited progression) silences what is sounding and re-enters at the new position.
        bool restart = ! wasPlaying || nextBeat < 0.0;
        if (! restart)
        {
            double gap = std::abs (startBeat - nextBeat);
            gap = std::min (gap, std::abs (loop - gap));
            restart = gap > std::max (1.0e-3, 0.25 * blockBeats);
        }
        const double blockStart = restart ? startBeat : nextBeat;
        const double blockEnd = positiveMod (startBeat + blockBeats, loop);

        if (restart)
        {
            chordSynth.allNotesOff (0, true);
            bassSynth.allNotesOff (0, true);
            auto chase = [blockStart, loop] (const std::vector<Note>& notes, juce::MidiBuffer& midi)
            {
                for (const auto& n : notes) // notes already under way at this position
                    if (n.start < blockStart && blockStart < std::min (n.start + n.length, loop - 0.001))
                        midi.addEvent (juce::MidiMessage::noteOn (1, n.pitch, (juce::uint8) n.velocity), 0);
            };
            chase (activePlayback->chords, chordMidi);
            chase (activePlayback->bass, bassMidi);
        }
        scheduleNotes (activePlayback->chords, chordMidi, blockStart, blockEnd, loop, numSamples);
        scheduleNotes (activePlayback->bass, bassMidi, blockStart, blockEnd, loop, numSamples);
        nextBeat = blockEnd;
    }
    else
    {
        if (wasPlaying)
        {
            chordSynth.allNotesOff (0, true);
            bassSynth.allNotesOff (0, true);
        }
        nextBeat = -1.0;
    }
    wasPlaying = shouldPlay;

    if (synthBuffer.getNumSamples() < numSamples)
        synthBuffer.setSize (2, numSamples, false, false, true);
    synthBuffer.clear();
    juce::AudioBuffer<float> synthView (synthBuffer.getArrayOfWritePointers(), 2, numSamples);
    chordSynth.renderNextBlock (synthView, chordMidi, 0, numSamples);
    bassSynth.renderNextBlock (synthView, bassMidi, 0, numSamples);
    const float gain = previewGain.load();
    for (int ch = 0; ch < numChannels && ch < getTotalNumOutputChannels(); ++ch)
        buffer.addFrom (ch, 0, synthBuffer, juce::jmin (ch, 1), 0, numSamples, gain);
}

//==============================================================================
// Session. Everything below takes stateLock, because hosts may save and restore state from
// threads other than the message thread.

void BacklineProcessor::setRecordArmed (bool shouldRecord)
{
    if (shouldRecord && ! recording.load())
    {
        // If the sample rate changed since the buffer was made, resize it before the take starts.
        const auto needed = (size_t) (currentSampleRate.load() * maxCaptureSeconds);
        if (capture.size() != needed)
        {
            capture.assign (needed, 0.0f);
            capturedSamples = 0;
        }
    }
    recordArmed = shouldRecord;
}

double BacklineProcessor::getRecordedSeconds() const { return capturedSamples.load() / captureSampleRate.load(); }

void BacklineProcessor::timerCallback()
{
    std::shared_ptr<const Playback> garbage;
    {
        const juce::SpinLock::ScopedLockType lock (playbackLock);
        garbage = std::move (releasedPlayback);
    }
    if (analysisPending.exchange (false))
        analyseCapture();
}

void BacklineProcessor::analyseCapture()
{
    const auto total = (size_t) juce::jmin (capturedSamples.load(), (int) capture.size());
    const double sampleRate = captureSampleRate.load(), newBpm = captureBpm.load(), bar = captureBeatsPerBar.load();
    const double samplesPerBeat = sampleRate * 60.0 / newBpm;

    // Start on the first bar line, unless that would leave less than a bar of audio.
    double skipBeats = captureSkipBeats.load();
    if ((double) total - skipBeats * samplesPerBeat < bar * samplesPerBeat)
        skipBeats = 0.0;
    const int offset = (int) (skipBeats * samplesPerBeat);
    const auto count = total - (size_t) offset;
    const float* audio = capture.data() + offset;
    const double beats = (double) count / samplesPerBeat;

    ChromaFrames newChroma;
    std::vector<Note> newMelody;
    if (beats >= bar)
    {
        newChroma = computeChroma (audio, count, sampleRate);
        newMelody = audioToNotes (audio, count, sampleRate, newBpm);
    }

    const juce::ScopedLock lock (stateLock);
    if (newChroma.frames.empty())
    {
        // The buffer no longer holds the previous take, so drop its analysis but keep the progression.
        lastTakeTooShort = true;
        chroma = {};
        segments.clear();
        ideas.clear();
        melodyNotes.clear();
        updatePlayback();
        return;
    }
    lastTakeTooShort = false;
    bpm = newBpm;
    beatsPerBar = bar;
    analysisOffset = offset;
    originPpq = captureStartPpq.load() + skipBeats;
    totalBeats = beats;
    chroma = std::move (newChroma);
    melodyNotes = std::move (newMelody);
    key = detectKey (chroma);
    rebuildIdeas();
}

void BacklineProcessor::rebuildIdeas()
{
    if (chroma.frames.empty())
    {
        updatePlayback();
        return;
    }
    const double beatsPerChord = getBeatsPerChord();
    const int numSegments = juce::jlimit (1, maxChords, juce::roundToInt (totalBeats / beatsPerChord));
    segments = segmentChroma (chroma, bpm, beatsPerChord, numSegments);
    ideas = suggestProgressions (segments, key, 4);
    selectedIdea = 0;
    if (! ideas.empty())
        progression = ideas.front();
    updatePlayback();
}

bool BacklineProcessor::hasProgression() const { const juce::ScopedLock lock (stateLock); return ! progression.empty(); }
bool BacklineProcessor::hasCapture() const     { const juce::ScopedLock lock (stateLock); return ! segments.empty(); }
Key BacklineProcessor::getKey() const          { const juce::ScopedLock lock (stateLock); return key; }
int BacklineProcessor::getNumIdeas() const     { const juce::ScopedLock lock (stateLock); return (int) ideas.size(); }
std::vector<Chord> BacklineProcessor::getProgression() const { const juce::ScopedLock lock (stateLock); return progression; }
double BacklineProcessor::getLoopBeats() const { const juce::ScopedLock lock (stateLock); return (double) progression.size() * getBeatsPerChord(); }

void BacklineProcessor::setKey (Key newKey)
{
    const juce::ScopedLock lock (stateLock);
    key = newKey;
    rebuildIdeas();
}

void BacklineProcessor::setChordRate (double barsPerChord)
{
    const juce::ScopedLock lock (stateLock);
    chordRate = barsPerChord <= 0.75 ? 0.5 : barsPerChord < 1.5 ? 1.0 : 2.0;
    rebuildIdeas();
}

void BacklineProcessor::selectIdea (int index)
{
    const juce::ScopedLock lock (stateLock);
    if (index >= 0 && index < (int) ideas.size())
    {
        selectedIdea = index;
        progression = ideas[(size_t) index];
    }
    updatePlayback();
}

std::vector<Chord> BacklineProcessor::getAlternatives (int segment) const
{
    const juce::ScopedLock lock (stateLock);
    if (segment >= 0 && segment < (int) segments.size())
        return rankChords (segments[(size_t) segment], key);
    return chordsInKey (key);
}

void BacklineProcessor::setChord (int segment, Chord chord)
{
    const juce::ScopedLock lock (stateLock);
    if (segment >= 0 && segment < (int) progression.size())
    {
        progression[(size_t) segment] = chord;
        updatePlayback();
    }
}

void BacklineProcessor::setChordRhythm (ChordRhythm r) { const juce::ScopedLock lock (stateLock); chordRhythm = r; updatePlayback(); }
void BacklineProcessor::setBassRhythm (BassRhythm r)   { const juce::ScopedLock lock (stateLock); bassRhythm = r; updatePlayback(); }

void BacklineProcessor::updatePlayback()
{
    const juce::ScopedLock stateGuard (stateLock);
    auto playback = std::make_shared<Playback>();
    playback->chords = renderChords (progression, getBeatsPerChord(), chordRhythm);
    playback->bass = renderBass (progression, getBeatsPerChord(), bassRhythm);
    playback->lengthBeats = (double) progression.size() * getBeatsPerChord();
    playback->originPpq = originPpq;

    std::shared_ptr<const Playback> old, garbage; // freed here, never on the audio thread
    {
        const juce::SpinLock::ScopedLockType lock (playbackLock);
        garbage = std::move (releasedPlayback);
        old = std::move (pendingPlayback);
        pendingPlayback = std::move (playback);
    }
    ++revision;
}

std::vector<Note> BacklineProcessor::getPartNotes (Part part) const
{
    const juce::ScopedLock lock (stateLock);
    switch (part)
    {
        case Part::Chords: return renderChords (progression, getBeatsPerChord(), chordRhythm);
        case Part::Bass:   return renderBass (progression, getBeatsPerChord(), bassRhythm);
        case Part::Melody: break;
    }
    return segments.empty() ? std::vector<Note>() : melodyNotes;
}

std::vector<float> BacklineProcessor::getWaveform (int numPoints) const
{
    // Samples below capturedSamples are never rewritten during a take, so this is safe to read live.
    std::vector<float> peaks ((size_t) juce::jmax (0, numPoints), 0.0f);
    const bool live = recording.load();
    int offset = 0;
    double span = 0.0; // samples shown across the full width
    {
        const juce::ScopedLock lock (stateLock);
        if (! live && ! segments.empty())
        {
            // After analysis the picture uses the same axis as the chords: one loop, starting on the bar line.
            offset = analysisOffset;
            span = getLoopBeats() * 60.0 / bpm * captureSampleRate.load();
        }
    }
    const int count = juce::jmin (capturedSamples.load(), (int) capture.size()) - offset;
    if (span <= 0.0)
        span = (double) count;
    if (count <= 0 || numPoints <= 0 || span <= 0.0)
        return peaks;
    const double perPoint = span / numPoints;
    const int stride = juce::jmax (1, (int) (perPoint / 48.0));
    for (int i = 0; i < numPoints; ++i)
    {
        const int begin = (int) (i * perPoint), end = juce::jmin (count, (int) ((i + 1) * perPoint) + 1);
        float peak = 0.0f;
        for (int k = begin; k < end; k += stride)
            peak = juce::jmax (peak, std::abs (capture[(size_t) (offset + k)]));
        peaks[(size_t) i] = juce::jmin (1.0f, peak);
    }
    return peaks;
}

juce::File BacklineProcessor::writeMidiFile (Part part)
{
    const auto notes = getPartNotes (part);
    const juce::String name = part == Part::Chords ? "Backline Chords"
                            : part == Part::Bass   ? "Backline Bass" : "Backline Audio to MIDI";
    if (notes.empty())
        return {};
    const auto bytes = backline::writeMidiFile (notes, getBpm());
    // One folder per plugin instance, so two instances never overwrite each other's file mid-drag.
    auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("Backline").getChildFile (juce::String::toHexString ((juce::pointer_sized_int) this));
    folder.createDirectory();
    auto file = folder.getChildFile (name + ".mid");
    return file.replaceWithData (bytes.data(), bytes.size()) ? file : juce::File();
}

//==============================================================================
void BacklineProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("Backline");
    {
        const juce::ScopedLock lock (stateLock);
        xml.setAttribute ("tonic", key.tonic);
        xml.setAttribute ("minor", key.minor);
        xml.setAttribute ("bpm", bpm);
        xml.setAttribute ("origin", originPpq);
        xml.setAttribute ("beatsPerBar", beatsPerBar);
        xml.setAttribute ("chordRate", chordRate);
        xml.setAttribute ("chordRhythm", (int) chordRhythm);
        xml.setAttribute ("bassRhythm", (int) bassRhythm);
        juce::StringArray chords;
        for (auto c : progression)
            chords.add (juce::String (c.root) + (c.quality == Quality::Minor ? "m" : "M"));
        xml.setAttribute ("progression", chords.joinIntoString (" "));
    }
    xml.setAttribute ("preview", previewEnabled.load());
    xml.setAttribute ("gain", (double) previewGain.load());
    copyXmlToBinary (xml, destData);
}

void BacklineProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("Backline"))
        return;
    previewEnabled = xml->getBoolAttribute ("preview", true);
    previewGain = (float) juce::jlimit (0.0, 1.0, xml->getDoubleAttribute ("gain", 0.7));

    const juce::ScopedLock lock (stateLock);
    key.tonic = juce::jlimit (0, 11, xml->getIntAttribute ("tonic"));
    key.minor = xml->getBoolAttribute ("minor");
    bpm = juce::jlimit (20.0, 999.0, xml->getDoubleAttribute ("bpm", 120.0));
    originPpq = xml->getDoubleAttribute ("origin");
    beatsPerBar = juce::jlimit (1.0, 32.0, xml->getDoubleAttribute ("beatsPerBar", 4.0));
    const double rate = xml->getDoubleAttribute ("chordRate", xml->getDoubleAttribute ("beatsPerChord", 4.0) / 4.0);
    chordRate = rate <= 0.75 ? 0.5 : rate < 1.5 ? 1.0 : 2.0;
    chordRhythm = (ChordRhythm) juce::jlimit (0, 3, xml->getIntAttribute ("chordRhythm"));
    bassRhythm = (BassRhythm) juce::jlimit (0, 4, xml->getIntAttribute ("bassRhythm", 1));

    // The captured audio is not stored in the session, only the chosen progression.
    chroma = {};
    melodyNotes.clear();
    segments.clear();
    ideas.clear();
    progression.clear();
    lastTakeTooShort = false;
    for (const auto& token : juce::StringArray::fromTokens (xml->getStringAttribute ("progression"), " ", ""))
        if (token.isNotEmpty() && (int) progression.size() < maxChords)
            progression.push_back ({ juce::jlimit (0, 11, token.getIntValue()),
                                     token.endsWithChar ('m') ? Quality::Minor : Quality::Major });
    updatePlayback();
}

juce::AudioProcessorEditor* BacklineProcessor::createEditor() { return new BacklineEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BacklineProcessor(); }
