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
    if (! recording.load())
        capture.assign ((size_t) (sampleRate * maxCaptureSeconds), 0.0f);
    chordSynth.setCurrentPlaybackSampleRate (sampleRate);
    bassSynth.setCurrentPlaybackSampleRate (sampleRate);
    synthBuffer.setSize (2, samplesPerBlock);
    chordMidi.ensureSize (4096);
    bassMidi.ensureSize (4096);
}

bool BacklineProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && layouts.getMainInputChannelSet() == out;
}

void BacklineProcessor::scheduleNotes (const std::vector<Note>& notes, juce::MidiBuffer& midi, double startBeat,
                                       double blockBeats, double loopBeats, int numSamples) const
{
    auto offsetFor = [&] (double beat, int& offset)
    {
        for (double candidate : { beat, beat + loopBeats })
            if (candidate >= startBeat && candidate < startBeat + blockBeats)
            {
                offset = juce::jlimit (0, numSamples - 1, (int) ((candidate - startBeat) / blockBeats * numSamples));
                return true;
            }
        return false;
    };

    int offset = 0;
    for (const auto& n : notes) // note-offs first so repeated pitches retrigger cleanly
        if (offsetFor (std::min (n.start + n.length, loopBeats - 0.001), offset))
            midi.addEvent (juce::MidiMessage::noteOff (1, n.pitch), offset);
    for (const auto& n : notes)
        if (offsetFor (n.start, offset))
            midi.addEvent (juce::MidiMessage::noteOn (1, n.pitch, (juce::uint8) n.velocity), offset);
}

void BacklineProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples(), numChannels = buffer.getNumChannels();
    for (int ch = getTotalNumInputChannels(); ch < numChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    // Transport: use the host's when there is one, otherwise a free-running clock (standalone).
    double hostBpm = 120.0, ppq = internalPpq;
    bool playing = true;
    if (auto* hostPlayHead = getPlayHead())
        if (auto position = hostPlayHead->getPosition())
        {
            playing = position->getIsPlaying();
            if (auto b = position->getBpm()) hostBpm = *b;
            if (auto p = position->getPpqPosition()) ppq = *p;
        }
    const double blockBeats = numSamples * hostBpm / (60.0 * currentSampleRate);
    internalPpq += blockBeats;

    // Capture the incoming audio (mono mix) while armed and the transport is running.
    if (recording.load() || recordArmed.load())
    {
        if (! recording.load() && playing && ! capture.empty())
        {
            capturedSamples = 0;
            captureStartPpq = ppq;
            captureBpm = hostBpm;
            recording = true;
        }
        if (recording.load())
        {
            int count = capturedSamples.load();
            const int room = (int) capture.size() - count;
            const int toCopy = (recordArmed.load() && playing) ? juce::jmin (numSamples, room) : 0;
            for (int i = 0; i < toCopy; ++i)
            {
                float mix = 0.0f;
                for (int ch = 0; ch < getTotalNumInputChannels() && ch < numChannels; ++ch)
                    mix += buffer.getSample (ch, i);
                capture[(size_t) (count + i)] = mix / (float) juce::jmax (1, getTotalNumInputChannels());
            }
            capturedSamples = count + toCopy;
            if (toCopy < numSamples) // stopped, disarmed or buffer full
            {
                recordArmed = false;
                recording = false;
                analysisPending = true;
            }
        }
    }

    // Preview the generated chords and bass in time with the transport.
    {
        const juce::SpinLock::ScopedTryLockType lock (playbackLock);
        if (lock.isLocked() && activePlayback != pendingPlayback)
        {
            activePlayback = pendingPlayback;
            chordSynth.allNotesOff (0, true);
            bassSynth.allNotesOff (0, true);
        }
    }

    chordMidi.clear();
    bassMidi.clear();
    const bool hasLoop = playing && activePlayback != nullptr && activePlayback->lengthBeats > 0.0;
    const double loop = hasLoop ? activePlayback->lengthBeats : 1.0;
    const double startBeat = hasLoop ? positiveMod (ppq - activePlayback->originPpq, loop) : -1.0;
    playheadBeats = startBeat;
    const bool shouldPlay = hasLoop && previewEnabled.load();
    if (shouldPlay)
    {
        scheduleNotes (activePlayback->chords, chordMidi, startBeat, blockBeats, loop, numSamples);
        scheduleNotes (activePlayback->bass, bassMidi, startBeat, blockBeats, loop, numSamples);
    }
    else if (wasPlaying)
    {
        chordSynth.allNotesOff (0, true);
        bassSynth.allNotesOff (0, true);
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
void BacklineProcessor::setRecordArmed (bool shouldRecord) { recordArmed = shouldRecord; }

double BacklineProcessor::getRecordedSeconds() const { return capturedSamples.load() / currentSampleRate; }

void BacklineProcessor::timerCallback()
{
    if (analysisPending.exchange (false))
        analyseCapture();
}

void BacklineProcessor::analyseCapture()
{
    const auto count = (size_t) capturedSamples.load();
    bpm = captureBpm.load();
    originPpq = captureStartPpq.load();
    totalBeats = (double) count / currentSampleRate * bpm / 60.0;
    chroma = computeChroma (capture.data(), count, currentSampleRate);
    melodyNotes = audioToNotes (capture.data(), count, currentSampleRate, bpm);
    if (chroma.frames.empty())
    {
        ++revision;
        return;
    }
    key = detectKey (chroma);
    rebuildIdeas();
}

void BacklineProcessor::rebuildIdeas()
{
    if (chroma.frames.empty())
        return;
    const int numSegments = juce::jmax (1, juce::roundToInt (totalBeats / beatsPerChord));
    segments = segmentChroma (chroma, bpm, beatsPerChord, numSegments);
    ideas = suggestProgressions (segments, key, 4);
    selectIdea (0);
}

void BacklineProcessor::setKey (Key newKey)
{
    key = newKey;
    if (hasCapture()) rebuildIdeas(); else updatePlayback();
}

void BacklineProcessor::setBeatsPerChord (double beats)
{
    beatsPerChord = beats;
    if (hasCapture()) rebuildIdeas(); else updatePlayback();
}

void BacklineProcessor::selectIdea (int index)
{
    if (index >= 0 && index < (int) ideas.size())
    {
        selectedIdea = index;
        progression = ideas[(size_t) index];
    }
    updatePlayback();
}

std::vector<Chord> BacklineProcessor::getAlternatives (int segment) const
{
    if (segment >= 0 && segment < (int) segments.size())
        return rankChords (segments[(size_t) segment], key);
    return chordsInKey (key);
}

void BacklineProcessor::setChord (int segment, Chord chord)
{
    if (segment >= 0 && segment < (int) progression.size())
    {
        progression[(size_t) segment] = chord;
        updatePlayback();
    }
}

void BacklineProcessor::updatePlayback()
{
    auto playback = std::make_shared<Playback>();
    playback->chords = renderChords (progression, beatsPerChord, chordRhythm);
    playback->bass = renderBass (progression, beatsPerChord, bassRhythm);
    playback->lengthBeats = (double) progression.size() * beatsPerChord;
    playback->originPpq = originPpq;
    {
        const juce::SpinLock::ScopedLockType lock (playbackLock);
        retiredPlayback = pendingPlayback; // keep the old one alive so the audio thread never frees it
        pendingPlayback = playback;
    }
    ++revision;
}

std::vector<Note> BacklineProcessor::getPartNotes (Part part) const
{
    switch (part)
    {
        case Part::Chords: return renderChords (progression, beatsPerChord, chordRhythm);
        case Part::Bass:   return renderBass (progression, beatsPerChord, bassRhythm);
        case Part::Melody: break;
    }
    return hasCapture() ? melodyNotes : std::vector<Note>();
}

std::vector<float> BacklineProcessor::getWaveform (int numPoints) const
{
    // Samples below capturedSamples are never rewritten during a take, so this is safe to read live.
    std::vector<float> peaks ((size_t) juce::jmax (0, numPoints), 0.0f);
    const int count = juce::jmin (capturedSamples.load(), (int) capture.size());
    if (count <= 0 || numPoints <= 0)
        return peaks;
    const double perPoint = (double) count / numPoints;
    const int stride = juce::jmax (1, (int) (perPoint / 48.0));
    for (int i = 0; i < numPoints; ++i)
    {
        const int begin = (int) (i * perPoint), end = juce::jmin (count, (int) ((i + 1) * perPoint) + 1);
        float peak = 0.0f;
        for (int k = begin; k < end; k += stride)
            peak = juce::jmax (peak, std::abs (capture[(size_t) k]));
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
    const auto bytes = backline::writeMidiFile (notes, bpm);
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name + ".mid");
    return file.replaceWithData (bytes.data(), bytes.size()) ? file : juce::File();
}

//==============================================================================
void BacklineProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("Backline");
    xml.setAttribute ("tonic", key.tonic);
    xml.setAttribute ("minor", key.minor);
    xml.setAttribute ("bpm", bpm);
    xml.setAttribute ("origin", originPpq);
    xml.setAttribute ("beatsPerChord", beatsPerChord);
    xml.setAttribute ("chordRhythm", (int) chordRhythm);
    xml.setAttribute ("bassRhythm", (int) bassRhythm);
    xml.setAttribute ("preview", previewEnabled.load());
    xml.setAttribute ("gain", (double) previewGain.load());
    juce::StringArray chords;
    for (auto c : progression)
        chords.add (juce::String (c.root) + (c.quality == Quality::Minor ? "m" : "M"));
    xml.setAttribute ("progression", chords.joinIntoString (" "));
    copyXmlToBinary (xml, destData);
}

void BacklineProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("Backline"))
        return;
    key.tonic = juce::jlimit (0, 11, xml->getIntAttribute ("tonic"));
    key.minor = xml->getBoolAttribute ("minor");
    bpm = juce::jmax (1.0, xml->getDoubleAttribute ("bpm", 120.0));
    originPpq = xml->getDoubleAttribute ("origin");
    beatsPerChord = juce::jlimit (1.0, 16.0, xml->getDoubleAttribute ("beatsPerChord", 4.0));
    chordRhythm = (ChordRhythm) juce::jlimit (0, 3, xml->getIntAttribute ("chordRhythm"));
    bassRhythm = (BassRhythm) juce::jlimit (0, 4, xml->getIntAttribute ("bassRhythm", 1));
    previewEnabled = xml->getBoolAttribute ("preview", true);
    previewGain = (float) juce::jlimit (0.0, 1.0, xml->getDoubleAttribute ("gain", 0.7));

    // The captured audio is not stored in the session, only the chosen progression.
    chroma = {};
    melodyNotes.clear();
    segments.clear();
    ideas.clear();
    progression.clear();
    for (const auto& token : juce::StringArray::fromTokens (xml->getStringAttribute ("progression"), " ", ""))
        if (token.isNotEmpty())
            progression.push_back ({ juce::jlimit (0, 11, token.getIntValue()),
                                     token.endsWithChar ('m') ? Quality::Minor : Quality::Major });
    updatePlayback();
}

juce::AudioProcessorEditor* BacklineProcessor::createEditor() { return new BacklineEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BacklineProcessor(); }
