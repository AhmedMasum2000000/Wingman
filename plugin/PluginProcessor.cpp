#include "PluginProcessor.h"
#include "PluginEditor.h"
#if M1D1_WEB_UI
 #include "WebEditor.h"
#endif
#include <cstring>

using namespace backline;

namespace
{
constexpr double maxCaptureSeconds = 120.0;   // recordings and loaded files
const char* const recordedTakeName = "Recorded take";

double positiveMod (double x, double m) { const double r = std::fmod (x, m); return r < 0 ? r + m : r; }

// The take's audio (from `offset`) as one channel.
std::vector<float> monoMix (const BacklineProcessor::TakeAudio& take, int offset)
{
    const int total = take.audio.getNumSamples(), channels = take.audio.getNumChannels();
    std::vector<float> mono ((size_t) juce::jmax (0, total - offset), 0.0f);
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* src = take.audio.getReadPointer (ch);
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] += src[(size_t) offset + i] / (float) channels;
    }
    return mono;
}

juce::String extensionList()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return formats.getWildcardForAllFormats().removeCharacters ("*.").replaceCharacter (',', ';');
}

juce::File midiExportFolder (const void* instance)
{
    // One folder per plugin instance, so two instances never overwrite each other's file mid-drag.
    auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("M1D1").getChildFile (juce::String::toHexString ((juce::pointer_sized_int) instance));
    folder.createDirectory();
    return folder;
}

juce::String partName (BacklineProcessor::Part part)
{
    return part == BacklineProcessor::Part::Chords ? "M1D1 Chords"
         : part == BacklineProcessor::Part::Bass   ? "M1D1 Bass" : "M1D1 Audio to MIDI";
}

// Where exported audio goes by default: a folder the user keeps, because DAWs refer to dropped audio there.
juce::File defaultExportFolder()
{
    auto base = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    if (! base.isDirectory())
        base = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    return base.getChildFile ("M1D1").getChildFile ("Exports");
}

// FNV-1a over the samples, to recognise audio that was exported before.
juce::uint64 hashOf (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    juce::uint64 hash = 14695981039346656037ull;
    auto mix = [&hash] (juce::uint64 value)
    {
        for (int i = 0; i < 8; ++i)
        {
            hash ^= (value >> (i * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    mix ((juce::uint64) std::llround (sampleRate * 1000.0));
    mix ((juce::uint64) audio.getNumChannels());
    mix ((juce::uint64) audio.getNumSamples());
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const float* data = audio.getReadPointer (ch);
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            juce::uint32 bits = 0;
            std::memcpy (&bits, data + i, sizeof (bits));
            mix (bits);
        }
    }
    return hash;
}
}

BacklineProcessor::BacklineProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    soundsFolder = instruments::defaultSoundsFolder();
    exportFolder = defaultExportFolder();
    for (int p = 0; p < 2; ++p)
    {
        requestedInstrument[p] = instruments::builtInName;
        loadedInstrument[p] = instruments::builtIn();
        activeSynth[p] = instruments::createSynth (loadedInstrument[p], p == 1, currentSampleRate.load());
    }
    startTimerHz (10);
}

BacklineProcessor::~BacklineProcessor()
{
    stopTimer();
    worker.removeAllJobs (true, 60000);
}

void BacklineProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    // Hosts call this often (some on every transport start) and with changing sample rates.
    // The record buffer is only allocated here the first time; the take is a separate copy and never touched.
    if (capture.empty())
        capture.assign ((size_t) (sampleRate * maxCaptureSeconds), 0.0f);
    for (auto& synth : activeSynth)
        synth->setCurrentPlaybackSampleRate (sampleRate);
    synthBuffer.setSize (2, samplesPerBlock);
    fadeBuffer.setSize (2, samplesPerBlock);
    chordMidi.ensureSize (4096);
    bassMidi.ensureSize (4096);
    haltPreviewAndTake();
    webEngine.prepare (sampleRate, samplesPerBlock);
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
    for (auto& synth : activeSynth)
        synth->allNotesOff (0, false);
    for (auto& left : fadeSamplesLeft)
        left = 0; // a replaced instrument still fading out falls silent (it is handed back later)
    wasPlaying = takeWasPlaying = false;
    nextBeat = takeNextBeat = -1.0;
    takeMain.active = takeTail.active = takeSwapWaiting = false;
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
    midi.clear();
   #if M1D1_WEB_UI
    webEngine.stopAll (midi);
   #endif
    if (midiOutWasOn)
    {
        for (int ch = chordMidiChannel; ch <= bassMidiChannel; ++ch)
            midi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        midiOutWasOn = false;
    }
}

// Sends the note events that fall in the block [blockStart, blockEnd) of a loop. Consecutive
// blocks must pass the previous blockEnd as their blockStart, so every event is sent exactly once
// even when it lands precisely on a block boundary.
void BacklineProcessor::scheduleNotes (const std::vector<Note>& notes, juce::MidiBuffer& midi, double blockStart,
                                       double blockEnd, double loopBeats, int numSamples)
{
    if (std::abs (blockEnd - blockStart) < 1.0e-12 || loopBeats <= 0.0 || numSamples <= 0)
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

void BacklineProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& hostMidi)
{
    hostMidi.clear(); // we take no MIDI in; whatever leaves this block is ours
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples(), numChannels = buffer.getNumChannels();
    for (int ch = getTotalNumInputChannels(); ch < numChannels; ++ch)
        buffer.clear (ch, 0, numSamples);
    if (numSamples <= 0)
        return;
   #if M1D1_WEB_UI
    // The M1D1 interface: its parts, in time with the host (the earlier engine has no interface in this build).
    webEngine.process (buffer, hostMidi, getPlayHead(), getTotalNumInputChannels());
    midiOutWasOn = true;
    return;
   #endif
    const double sampleRate = currentSampleRate.load();

    // Transport: the host's when it provides musical time, otherwise our own clock at the manual
    // tempo. (The standalone app and some simple hosts supply a playhead with no tempo or position.)
    double tempo = manualBpm.load(), ppq = internalPpq, hostBeatsPerBar = 4.0, barStart = 0.0;
    bool playing = transportPlaying.load(), hasBarStart = false, host = false, hostHasPpq = false;
    if (auto* hostPlayHead = getPlayHead())
        if (auto position = hostPlayHead->getPosition())
            if (position->getPpqPosition().hasValue() || position->getBpm().hasValue())
            {
                host = true;
                playing = position->getIsPlaying();
                tempo = defaultTempo;
                if (auto b = position->getBpm()) tempo = juce::jlimit (20.0, 999.0, *b);
                if (auto p = position->getPpqPosition()) { ppq = *p; hostHasPpq = true; }
                if (auto sig = position->getTimeSignature())
                    hostBeatsPerBar = juce::jmax (1.0, sig->numerator * 4.0 / juce::jmax (1, sig->denominator));
                if (auto b = position->getPpqPositionOfLastBarStart()) { barStart = *b; hasBarStart = true; }
            }
    hostTransport = host;
    if (host)
    {
        lastHostBpm = tempo;
        lastHostBeatsPerBar = hostBeatsPerBar;
    }
    else if (rewindRequested.exchange (false))
        ppq = internalPpq = rewindTarget.load();
    const double blockBeats = numSamples * tempo / (60.0 * sampleRate);
    if (! hostHasPpq && playing)
        internalPpq += blockBeats;

    // Capture the incoming audio (mono mix) while armed and the transport is running.
    if (recording.load() || recordArmed.load())
    {
        auto startTake = [&]
        {
            // Recording may start mid-bar; remember how far it is to the next bar line.
            const double intoBar = positiveMod (hasBarStart ? ppq - barStart : ppq, hostBeatsPerBar);
            captureSkipBeats = (! host || intoBar < 0.05) ? 0.0 : hostBeatsPerBar - intoBar;
            capturedSamples = 0;
            captureStartPpq = ppq;
            captureBpm = tempo;
            captureBeatsPerBar = hostBeatsPerBar;
            captureSampleRate = sampleRate;
            captureHadHostTransport = host;
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
            const bool jumped = (host && std::abs (ppq - expectedRecordPpq) > std::max (0.01, 0.5 * blockBeats))
                             || std::abs (tempo - captureBpm.load()) > 0.01;
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

    // Pick up a newly published progression and instruments. What they replace is handed back
    // for the message thread to free.
    {
        const juce::SpinLock::ScopedTryLockType lock (playbackLock);
        if (lock.isLocked())
        {
            if (activePlayback != pendingPlayback && releasedPlayback == nullptr)
            {
                const auto* oldTake = activePlayback != nullptr ? activePlayback->take.get() : nullptr;
                const auto* newTake = pendingPlayback != nullptr ? pendingPlayback->take.get() : nullptr;
                if (oldTake != newTake && (takeMain.active || takeTail.active))
                {
                    // A different take: fade the old one out first; the swap happens once it is silent,
                    // because the fading reader still reads the old take's audio.
                    if (takeMain.active)
                    {
                        takeTail = takeMain;
                        takeTail.fade = takeTail.fadeLength = juce::jmax (1, (int) (0.01 * sampleRate));
                        takeMain.active = false;
                        takeNextBeat = -1.0;
                    }
                    takeSwapWaiting = true;
                }
                else
                {
                    takeSwapWaiting = false;
                    const bool remapped = oldTake == newTake && oldTake != nullptr
                        && (std::abs (activePlayback->takeStartSample - pendingPlayback->takeStartSample) > 1.0e-6
                            || std::abs (activePlayback->takeSamplesPerBeat - pendingPlayback->takeSamplesPerBeat) > 1.0e-9
                            || std::abs (activePlayback->lengthBeats - pendingPlayback->lengthBeats) > 1.0e-9
                            || std::abs (activePlayback->originPpq - pendingPlayback->originPpq) > 1.0e-9);
                    releasedPlayback = std::move (activePlayback);
                    activePlayback = pendingPlayback;
                    nextBeat = -1.0; // restart the notes from the current position
                    if (remapped)
                        takeNextBeat = -1.0; // re-enter the take at the new mapping (with a crossfade)
                }
            }
            for (int p = 0; p < 2; ++p)
            {
                // A replaced instrument that has faded out goes back to be freed.
                if (fadingSynth[p] != nullptr && fadeSamplesLeft[p] <= 0 && releasedSynth[p] == nullptr)
                    releasedSynth[p] = std::move (fadingSynth[p]);
                if (pendingSynth[p] != nullptr && fadingSynth[p] == nullptr)
                {
                    // The old instrument lets its notes go and fades out over 50 ms while the new
                    // one picks up the notes already under way, so the change does not click.
                    fadingSynth[p] = std::move (activeSynth[p]);
                    fadingSynth[p]->allNotesOff (0, true);
                    fadeSamples[p] = fadeSamplesLeft[p] = juce::jmax (1, (int) (0.05 * sampleRate));
                    activeSynth[p] = std::move (pendingSynth[p]);
                    if (std::abs (activeSynth[p]->getSampleRate() - sampleRate) > 1.0e-6)
                        activeSynth[p]->setCurrentPlaybackSampleRate (sampleRate);
                    nextBeat = -1.0;
                }
            }
        }
    }

    // Preview the generated chords and bass in time with the transport.
    chordMidi.clear();
    bassMidi.clear();
    bool flushHost = false;
    const bool hasLoop = playing && activePlayback != nullptr && activePlayback->lengthBeats > 0.0;
    const double loop = hasLoop ? activePlayback->lengthBeats : 1.0;
    const double startBeat = hasLoop ? positiveMod (ppq - activePlayback->originPpq, loop) : -1.0;
    playheadBeats = startBeat;
    const bool shouldPlay = hasLoop && previewEnabled.load();
    auto& chordSynth = *activeSynth[0];
    auto& bassSynth = *activeSynth[1];
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
            flushHost = true;
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
            flushHost = true;
            chordSynth.allNotesOff (0, true);
            bassSynth.allNotesOff (0, true);
        }
        nextBeat = -1.0;
    }
    wasPlaying = shouldPlay;

    // MIDI out: the same notes go to the host, chords on channel 1 and bass on channel 2, so the
    // user can route them to any instrument. A restart or stop first releases whatever was held.
    if (midiOutEnabled.load())
    {
        if (flushHost)
            for (int ch = chordMidiChannel; ch <= bassMidiChannel; ++ch)
                hostMidi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        hostMidi.addEvents (chordMidi, 0, numSamples, 0);
        for (const auto meta : bassMidi)
        {
            auto message = meta.getMessage();
            message.setChannel (bassMidiChannel);
            hostMidi.addEvent (message, meta.samplePosition);
        }
        midiOutWasOn = true;
    }
    else if (midiOutWasOn)
    {
        for (int ch = chordMidiChannel; ch <= bassMidiChannel; ++ch)
            hostMidi.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
        midiOutWasOn = false;
    }

    if (synthBuffer.getNumSamples() < numSamples)
        synthBuffer.setSize (2, numSamples, false, false, true);
    synthBuffer.clear();
    juce::AudioBuffer<float> synthView (synthBuffer.getArrayOfWritePointers(), 2, numSamples);
    chordSynth.renderNextBlock (synthView, chordMidi, 0, numSamples);
    bassSynth.renderNextBlock (synthView, bassMidi, 0, numSamples);
    for (int p = 0; p < 2; ++p)
        if (fadingSynth[p] != nullptr && fadeSamplesLeft[p] > 0)
        {
            if (fadeBuffer.getNumSamples() < numSamples)
                fadeBuffer.setSize (2, numSamples, false, false, true);
            juce::AudioBuffer<float> fadeView (fadeBuffer.getArrayOfWritePointers(), 2, numSamples);
            fadeView.clear();
            fadingSynth[p]->renderNextBlock (fadeView, noMidi, 0, numSamples);
            const int count = juce::jmin (numSamples, fadeSamplesLeft[p]);
            const float from = (float) fadeSamplesLeft[p] / (float) fadeSamples[p];
            const float to = (float) (fadeSamplesLeft[p] - count) / (float) fadeSamples[p];
            for (int ch = 0; ch < 2; ++ch)
                synthView.addFromWithRamp (ch, 0, fadeView.getReadPointer (ch), count, from, to);
            fadeSamplesLeft[p] -= count;
        }
    const float gain = previewGain.load();
    for (int ch = 0; ch < numChannels && ch < getTotalNumOutputChannels(); ++ch)
        buffer.addFrom (ch, 0, synthBuffer, juce::jmin (ch, 1), 0, numSamples, gain);

    const bool takeOn = hasLoop && playTake.load() && ! takeSwapWaiting && activePlayback->take != nullptr
                     && activePlayback->takeSamplesPerBeat > 0.0;
    renderTake (buffer, numChannels, numSamples, takeOn, startBeat, blockBeats);
}

// Plays the take in a loop, locked to the same loop position as the chords. The loop region is
// read with interpolation (so a take at another sample rate or tempo is resampled). Wherever the
// read position jumps (loop end, seek, start, stop, new mapping) the old position carries on for
// 10 ms fading out while the new one fades in over 2 ms, so there is no click.
void BacklineProcessor::renderTake (juce::AudioBuffer<float>& buffer, int numChannels, int numSamples, bool takeOn,
                                    double startBeat, double blockBeats)
{
    const double sampleRate = currentSampleRate.load();
    const int fadeIn = juce::jmax (1, (int) (0.002 * sampleRate)), fadeOut = juce::jmax (1, (int) (0.01 * sampleRate));
    auto fadeOutMain = [&]
    {
        if (takeMain.active)
        {
            takeTail = takeMain;
            takeTail.fade = takeTail.fadeLength = fadeOut;
        }
        takeMain.active = false;
    };

    double loopStart = 0.0, loopSpan = 0.0;
    if (takeOn)
    {
        const auto& pb = *activePlayback;
        const double loop = pb.lengthBeats, perBeat = pb.takeSamplesPerBeat;
        loopStart = pb.takeStartSample;
        loopSpan = loop * perBeat;
        const double target = loopStart + startBeat * perBeat;
        bool restart = ! takeWasPlaying || takeNextBeat < 0.0 || ! takeMain.active || takeMain.take != pb.take.get();
        if (! restart)
        {
            double gap = std::abs (startBeat - takeNextBeat);
            gap = std::min (gap, std::abs (loop - gap));
            restart = gap > std::max (1.0e-3, 0.25 * blockBeats);
        }
        if (restart)
        {
            fadeOutMain();
            takeMain.take = pb.take.get();
            takeMain.position = target;
            takeMain.fade = 0;
            takeMain.fadeLength = fadeIn;
            takeMain.active = true;
        }
        else
        {
            // Stay on the transport: correct the tiny drift, measured round the loop so that a
            // position just past the loop end (about to wrap) stays there.
            double diff = target - takeMain.position;
            diff -= loopSpan * std::round (diff / loopSpan);
            takeMain.position += diff;
        }
        takeMain.increment = perBeat * blockBeats / numSamples;
        takeNextBeat = positiveMod (startBeat + blockBeats, loop);
    }
    else
    {
        fadeOutMain();
        takeNextBeat = -1.0;
    }
    takeWasPlaying = takeOn;
    if (! takeMain.active && ! takeTail.active)
        return;

    const int outputs = juce::jmin (numChannels, getTotalNumOutputChannels());
    const float level = takeGain.load();
    auto addReader = [&] (TakeReader& reader, int index, float gain)
    {
        const auto& audio = reader.take->audio;
        const int length = audio.getNumSamples(), channels = audio.getNumChannels();
        // Fade the very end of the take, in case the loop runs past it.
        const float endGain = (float) juce::jlimit (0.0, 1.0, ((double) length - reader.position) / fadeOut);
        if (endGain <= 0.0f || reader.position < -4.0)
            return;
        for (int ch = 0; ch < outputs; ++ch)
            buffer.addSample (ch, index, gain * endGain * instruments::interpolate (audio.getReadPointer (juce::jmin (ch, channels - 1)),
                                                                            length, reader.position));
    };

    for (int i = 0; i < numSamples; ++i)
    {
        if (takeMain.active)
        {
            if (loopSpan > 0.0 && takeMain.position >= loopStart + loopSpan)
            {
                fadeOutMain(); // the audio after the loop end carries on, fading out...
                takeMain.active = true;
                takeMain.position -= loopSpan; // ...while the loop starts again
                takeMain.fade = 0;
                takeMain.fadeLength = fadeIn;
            }
            const float in = juce::jmin (1.0f, (float) (takeMain.fade + 1) / (float) takeMain.fadeLength);
            addReader (takeMain, i, level * in);
            if (takeMain.fade < takeMain.fadeLength)
                ++takeMain.fade;
            takeMain.position += takeMain.increment;
        }
        if (takeTail.active)
        {
            addReader (takeTail, i, level * (float) takeTail.fade / (float) takeTail.fadeLength);
            takeTail.position += takeTail.increment;
            if (--takeTail.fade <= 0)
                takeTail.active = false;
        }
    }
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
        if (! hostTransport.load())
            transportPlaying = true; // our own transport must run to record
    }
    recordArmed = shouldRecord;
}

double BacklineProcessor::getRecordedSeconds() const
{
    if (recording.load())
        return capturedSamples.load() / captureSampleRate.load();
    const juce::ScopedLock lock (stateLock);
    return take != nullptr ? take->audio.getNumSamples() / take->sampleRate : 0.0;
}

void BacklineProcessor::post (std::function<void()> resultForMessageThread)
{
    const juce::ScopedLock lock (resultLock);
    finishedWork.push_back (std::move (resultForMessageThread));
}

void BacklineProcessor::timerCallback()
{
    std::shared_ptr<const Playback> garbage;
    std::unique_ptr<juce::Synthesiser> oldSynths[2];
    {
        const juce::SpinLock::ScopedLockType lock (playbackLock);
        garbage = std::move (releasedPlayback);
        for (int p = 0; p < 2; ++p)
            oldSynths[p] = std::move (releasedSynth[p]);
    }
    std::vector<std::function<void()>> work;
    {
        const juce::ScopedLock lock (resultLock);
        work.swap (finishedWork);
    }
    for (auto& w : work)
        w();
    if (analysisPending.exchange (false))
        analyseCapture();
}

BacklineProcessor::Analysis BacklineProcessor::analyseTake (std::shared_ptr<const TakeAudio> takeAudio, double newBpm,
                                                            double bar, int offsetSamples, double origin,
                                                            const ChromaFrames* knownChroma)
{
    Analysis a;
    a.take = takeAudio;
    a.bpm = newBpm;
    a.beatsPerBar = bar;
    a.offset = offsetSamples;
    a.originPpq = origin;
    if (takeAudio == nullptr)
        return a;
    const double sampleRate = takeAudio->sampleRate, samplesPerBeat = sampleRate * 60.0 / newBpm;
    const int count = juce::jmax (0, takeAudio->audio.getNumSamples() - offsetSamples);
    a.totalBeats = count / samplesPerBeat;
    if (a.totalBeats < bar)
        return a;

    std::vector<float> mixed;
    const float* audio = nullptr;
    if (takeAudio->audio.getNumChannels() == 1)
        audio = takeAudio->audio.getReadPointer (0) + offsetSamples;
    else
    {
        mixed = monoMix (*takeAudio, offsetSamples);
        audio = mixed.data();
    }
    a.chroma = knownChroma != nullptr ? *knownChroma : computeChroma (audio, (size_t) count, sampleRate);
    a.melody = audioToNotes (audio, (size_t) count, sampleRate, newBpm);
    return a;
}

void BacklineProcessor::analyseCapture()
{
    const int total = juce::jmin (capturedSamples.load(), (int) capture.size());
    const double sampleRate = captureSampleRate.load(), newBpm = captureBpm.load(), bar = captureBeatsPerBar.load();
    const double samplesPerBeat = sampleRate * 60.0 / newBpm;

    // Start on the first bar line, unless that would leave less than a bar of audio.
    double skipBeats = captureSkipBeats.load();
    if ((double) total - skipBeats * samplesPerBeat < bar * samplesPerBeat)
        skipBeats = 0.0;

    auto recorded = std::make_shared<TakeAudio>();
    recorded->audio.setSize (1, total);
    if (total > 0)
        recorded->audio.copyFrom (0, 0, capture.data(), total);
    recorded->sampleRate = sampleRate;
    recorded->name = recordedTakeName;
    auto analysis = analyseTake (recorded, newBpm, bar, (int) (skipBeats * samplesPerBeat),
                                 captureStartPpq.load() + skipBeats, nullptr);
    installAnalysis (std::move (analysis), true, captureHadHostTransport.load());
}

void BacklineProcessor::installAnalysis (Analysis&& analysis, bool newTake, bool fromHostRecording)
{
    const juce::ScopedLock lock (stateLock);
    ++analysisTicket; // anything still being re-analysed is out of date now
    if (! analysis.ok())
    {
        // A take too short to use. The old take is gone, so drop its analysis but keep the progression.
        lastTakeTooShort = true;
        take = nullptr;
        chroma = {};
        segments.clear();
        ideas.clear();
        melodyNotes.clear();
        estimatedBpm = 0.0;
        updatePlayback();
        return;
    }
    lastTakeTooShort = false;
    take = analysis.take;
    bpm = analysis.bpm;
    beatsPerBar = analysis.beatsPerBar;
    analysisOffset = analysis.offset;
    originPpq = analysis.originPpq;
    totalBeats = analysis.totalBeats;
    chroma = std::move (analysis.chroma);
    melodyNotes = std::move (analysis.melody);
    if (newTake)
    {
        key = detectKey (chroma);
        estimatedBpm = analysis.estimatedBpm;
        playTake = take->fromFile || ! fromHostRecording;
    }
    // The same take at another tempo: fresh ideas, but chords the user has edited stay as they are.
    rebuildIdeas (! newTake && progressionEdited);
}

void BacklineProcessor::rebuildIdeas (bool keepProgression)
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
    if (keepProgression && ! progression.empty())
        selectedIdea = juce::jlimit (0, juce::jmax (0, (int) ideas.size() - 1), selectedIdea);
    else
    {
        selectedIdea = 0;
        if (! ideas.empty())
        {
            progression = toProgression (ideas.front(), beatsPerChord);
            if (sevenths)
                progression = withSevenths (progression, key);
            progressionEdited = false;
        }
    }
    updatePlayback();
}

bool BacklineProcessor::hasProgression() const { const juce::ScopedLock lock (stateLock); return ! progression.empty(); }
bool BacklineProcessor::hasCapture() const     { const juce::ScopedLock lock (stateLock); return ! segments.empty(); }
bool BacklineProcessor::hasTake() const        { const juce::ScopedLock lock (stateLock); return take != nullptr; }
Key BacklineProcessor::getKey() const          { const juce::ScopedLock lock (stateLock); return key; }
int BacklineProcessor::getNumIdeas() const     { const juce::ScopedLock lock (stateLock); return (int) ideas.size(); }
Progression BacklineProcessor::getProgression() const { const juce::ScopedLock lock (stateLock); return progression; }
std::vector<Chord> BacklineProcessor::getChords() const { const juce::ScopedLock lock (stateLock); return chordsOf (progression); }
double BacklineProcessor::getLoopBeats() const { const juce::ScopedLock lock (stateLock); return progressionLength (progression); }
int BacklineProcessor::getChordIndexAt (double beat) const { const juce::ScopedLock lock (stateLock); return chordIndexAt (progression, beat); }

juce::String BacklineProcessor::getTakeName() const
{
    const juce::ScopedLock lock (stateLock);
    return take != nullptr ? take->name : juce::String();
}

bool BacklineProcessor::isTakeFromFile() const
{
    const juce::ScopedLock lock (stateLock);
    return take != nullptr && take->fromFile;
}

double BacklineProcessor::getGridBeats() const
{
    const juce::ScopedLock lock (stateLock);
    // Half-bar chords in an odd meter (1.5 beats in 3/4) need a half-beat grid to stay editable.
    const double halfBar = beatsPerBar * 0.5;
    return chordRate < 0.75 && std::abs (halfBar - std::round (halfBar)) > 1.0e-9 ? 0.5 : 1.0;
}

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
        progression = toProgression (ideas[(size_t) index], getBeatsPerChord());
        if (sevenths)
            progression = withSevenths (progression, key);
        progressionEdited = false;
    }
    updatePlayback();
}

//==============================================================================
// Chord edits

bool BacklineProcessor::editProgression (const std::function<Progression (const Progression&)>& edit)
{
    const juce::ScopedLock lock (stateLock);
    if (progression.empty())
        return false;
    auto next = edit (progression);
    if (next == progression || ! isValidProgression (next) || (int) next.size() > maxChords)
        return false;
    progression = std::move (next);
    progressionEdited = true;
    updatePlayback();
    return true;
}

bool BacklineProcessor::setChord (int index, Chord chord)
{
    if (index < 0)
        return false;
    chord.root = ((chord.root % 12) + 12) % 12;
    if (chord.bass >= 12 || chord.bass < -1)
        chord.bass = ((chord.bass % 12) + 12) % 12;
    return editProgression ([&] (const Progression& p) { return backline::setChord (p, (size_t) index, chord); });
}

bool BacklineProcessor::splitChord (int index)
{
    const double grid = getGridBeats();
    return index >= 0 && editProgression ([&] (const Progression& p)
    {
        return (int) p.size() < maxChords ? backline::splitChord (p, (size_t) index, grid) : p;
    });
}

bool BacklineProcessor::moveChordBoundary (int boundary, double beat)
{
    const double grid = getGridBeats();
    return boundary >= 0 && editProgression ([&] (const Progression& p) { return moveBoundary (p, (size_t) boundary, beat, grid); });
}

bool BacklineProcessor::deleteChord (int index)
{
    return index >= 0 && editProgression ([&] (const Progression& p) { return backline::deleteChord (p, (size_t) index); });
}

bool BacklineProcessor::insertPassingChord (int index)
{
    const Key k = getKey();
    return index >= 0 && editProgression ([&] (const Progression& p)
    {
        return (int) p.size() < maxChords ? backline::insertPassingChord (p, (size_t) index, k) : p;
    });
}

Chroma BacklineProcessor::chromaForSpan (double startBeat, double lengthBeats) const
{
    Chroma sum {};
    const double secondsPerBeat = 60.0 / bpm, from = startBeat * secondsPerBeat, to = (startBeat + lengthBeats) * secondsPerBeat;
    for (size_t i = 0; i < chroma.frames.size(); ++i)
    {
        const double centre = (double) i * chroma.hopSeconds + chroma.windowSeconds * 0.5;
        if (centre >= from && centre < to)
            for (size_t k = 0; k < 12; ++k)
                sum[k] += chroma.frames[i][k];
    }
    float total = 0.0f;
    for (float v : sum) total += v;
    if (total > 1.0e-6f)
        for (float& v : sum) v /= total;
    return sum;
}

std::vector<Chord> BacklineProcessor::getAlternatives (int index) const
{
    const juce::ScopedLock lock (stateLock);
    std::vector<Chord> result;
    if (index >= 0 && index < (int) progression.size() && ! chroma.frames.empty())
    {
        const auto& event = progression[(size_t) index];
        result = rankChords (chromaForSpan (event.start, event.length), key);
    }
    else
        result = chordsInKey (key);
    if (sevenths)
        for (auto& c : result)
            c = seventhChord (c, key);
    return result;
}

std::vector<Chord> BacklineProcessor::getVariations (int index) const
{
    const juce::ScopedLock lock (stateLock);
    if (index < 0 || index >= (int) progression.size())
        return {};
    return chordVariations (progression[(size_t) index].chord, key);
}

void BacklineProcessor::setSevenths (bool on)
{
    const juce::ScopedLock lock (stateLock);
    if (on == sevenths)
        return;
    sevenths = on;
    progression = on ? withSevenths (progression, key) : withoutSevenths (progression);
    updatePlayback();
}

void BacklineProcessor::setChordRhythm (ChordRhythm r) { const juce::ScopedLock lock (stateLock); chordRhythm = r; updatePlayback(); }
void BacklineProcessor::setBassRhythm (BassRhythm r)   { const juce::ScopedLock lock (stateLock); bassRhythm = r; updatePlayback(); }

void BacklineProcessor::updatePlayback()
{
    const juce::ScopedLock stateGuard (stateLock);
    auto playback = std::make_shared<Playback>();
    playback->chords = renderChords (progression, chordRhythm, beatsPerBar);
    playback->bass = renderBass (progression, bassRhythm);
    playback->lengthBeats = progressionLength (progression);
    playback->originPpq = originPpq;
    if (take != nullptr && bpm > 0.0)
    {
        playback->take = take;
        playback->takeStartSample = analysisOffset;
        playback->takeSamplesPerBeat = take->sampleRate * 60.0 / bpm;
    }

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
        case Part::Chords: return renderChords (progression, chordRhythm, beatsPerBar);
        case Part::Bass:   return renderBass (progression, bassRhythm);
        case Part::Melody: break;
    }
    return segments.empty() ? std::vector<Note>() : melodyNotes;
}

std::vector<float> BacklineProcessor::getWaveform (int numPoints) const
{
    std::vector<float> peaks ((size_t) juce::jmax (0, numPoints), 0.0f);
    if (numPoints <= 0)
        return peaks;
    auto fill = [&] (int count, double span, auto&& peakOf)
    {
        if (count <= 0 || span <= 0.0)
            return;
        const double perPoint = span / numPoints;
        const int stride = juce::jmax (1, (int) (perPoint / 48.0));
        for (int i = 0; i < numPoints; ++i)
        {
            const int begin = (int) (i * perPoint), end = juce::jmin (count, (int) ((i + 1) * perPoint) + 1);
            float peak = 0.0f;
            for (int k = begin; k < end; k += stride)
                peak = juce::jmax (peak, peakOf (k));
            peaks[(size_t) i] = juce::jmin (1.0f, peak);
        }
    };

    if (recording.load())
    {
        // Samples below capturedSamples are never rewritten during a take, so this is safe to read live.
        const int count = juce::jmin (capturedSamples.load(), (int) capture.size());
        fill (count, (double) count, [this] (int k) { return std::abs (capture[(size_t) k]); });
        return peaks;
    }

    std::shared_ptr<const TakeAudio> shown;
    int offset = 0;
    double span = 0.0; // samples shown across the full width
    {
        const juce::ScopedLock lock (stateLock);
        shown = take;
        if (shown != nullptr && ! segments.empty())
        {
            // After analysis the picture uses the same axis as the chords: one loop, starting on the bar line.
            offset = analysisOffset;
            span = progressionLength (progression) * 60.0 / bpm * shown->sampleRate;
        }
    }
    if (shown == nullptr)
        return peaks;
    const auto& audio = shown->audio;
    const int count = audio.getNumSamples() - offset;
    fill (count, span > 0.0 ? span : (double) count, [&] (int k)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            peak = juce::jmax (peak, std::abs (audio.getSample (ch, offset + k)));
        return peak;
    });
    return peaks;
}

//==============================================================================
// Audio files, tempo and transport

bool BacklineProcessor::isAudioFile (const juce::File& file)
{
    static const juce::String extensions = extensionList();
    return file.hasFileExtension (extensions);
}

juce::String BacklineProcessor::getAudioFileWildcard()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return formats.getWildcardForAllFormats().replaceCharacter (',', ';');
}

std::shared_ptr<BacklineProcessor::TakeAudio> BacklineProcessor::decodeFile (const juce::File& file, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = "The file could not be found.";
        return nullptr;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "This file type cannot be opened. Try WAV, AIFF, FLAC or OGG.";
        return nullptr;
    }
    if (reader->sampleRate < 1000.0 || reader->lengthInSamples <= 0 || reader->numChannels == 0)
    {
        error = "The file holds no audio.";
        return nullptr;
    }
    const auto length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * maxCaptureSeconds));
    auto decoded = std::make_shared<TakeAudio>();
    decoded->audio.setSize (reader->numChannels > 1 ? 2 : 1, length);
    if (! reader->read (&decoded->audio, 0, length, 0, true, reader->numChannels > 1))
    {
        error = "The file could not be read.";
        return nullptr;
    }
    decoded->sampleRate = reader->sampleRate;
    decoded->name = file.getFileName();
    decoded->fromFile = true;
    return decoded;
}

void BacklineProcessor::loadAudioFile (const juce::File& file)
{
    int ticket = 0;
    {
        const juce::ScopedLock lock (stateLock);
        ticket = ++loadTicket;
        loadError.clear();
    }
    ++pendingLoads;
    ++revision;

    // Runs on the message thread when the worker is done (or has given up).
    auto finish = [this, ticket] (Analysis analysis, juce::String error, bool withHost)
    {
        {
            const juce::ScopedLock lock (stateLock);
            if (ticket == loadTicket)
            {
                if (analysis.take == nullptr)
                    loadError = error.isNotEmpty() ? error : juce::String ("The file could not be opened.");
                else if (! analysis.ok())
                    loadError = "The file is too short. It needs at least one bar of audio.";
                else
                {
                    if (! withHost)
                    {
                        manualBpm = analysis.bpm; // the estimate (or the manual tempo) becomes the tempo
                        rewindTarget = analysis.originPpq;
                        rewindRequested = true;   // play the new loop from its start
                    }
                    installAnalysis (std::move (analysis), true, false);
                }
            }
        }
        --pendingLoads;
        ++revision;
    };

    // Worker: tempo estimate (own transport only) and analysis of a decoded file.
    auto analyse = [this, finish] (std::shared_ptr<const TakeAudio> decoded)
    {
        const bool withHost = hostTransport.load();
        double estimate = 0.0;
        if (! withHost)
        {
            const auto mono = monoMix (*decoded, 0);
            estimate = estimateTempo (mono.data(), mono.size(), decoded->sampleRate);
        }
        const double useBpm = withHost ? lastHostBpm.load() : estimate > 0.0 ? estimate : manualBpm.load();
        auto analysis = analyseTake (decoded, useBpm, withHost ? lastHostBeatsPerBar.load() : 4.0, 0, 0.0, nullptr);
        analysis.estimatedBpm = estimate;
        post ([finish, analysis, withHost] { finish (analysis, {}, withHost); });
    };

    worker.addJob ([this, file, finish, analyse]
    {
        juce::String error;
        if (auto decoded = decodeFile (file, error))
        {
            analyse (decoded);
            return;
        }
        // Some platform decoders (Windows Media for MP3) only work on the message thread, so try
        // once more there before giving up.
        const bool retry = isAudioFile (file) && file.existsAsFile();
        post ([this, file, finish, analyse, error, retry]
        {
            juce::String secondError = error;
            if (retry)
                if (auto decoded = decodeFile (file, secondError))
                {
                    worker.addJob ([analyse, decoded] { analyse (decoded); });
                    return;
                }
            finish ({}, secondError, hostTransport.load());
        });
    });
}

double BacklineProcessor::getTempo() const
{
    return hostTransport.load() ? lastHostBpm.load() : manualBpm.load();
}

void BacklineProcessor::setManualTempo (double newBpm)
{
    newBpm = juce::jlimit (minManualTempo, maxManualTempo, newBpm);
    const bool changed = std::abs (newBpm - manualBpm.exchange (newBpm)) > 1.0e-9;
    if (hostTransport.load())
    {
        if (changed)
            ++revision;
        return;
    }
    {
        const juce::ScopedLock lock (stateLock);
        if (take == nullptr || chroma.frames.empty() || recording.load())
        {
            bpm = newBpm; // nothing to re-analyse; exports use the new tempo
            if (changed)
                ++revision;
            return;
        }
        if (std::abs (bpm - newBpm) <= 1.0e-9)
        {
            if (changed)
                ++revision;
            return; // the take is analysed at this tempo already: nothing to redo, nothing lost
        }
    }
    requestReanalysis();
}

// Re-analyses the current take at the manual tempo (own transport only). The take plays at the
// new tempo straight away; the ideas follow when the analysis is done.
void BacklineProcessor::requestReanalysis()
{
    std::shared_ptr<const TakeAudio> current;
    ChromaFrames known;
    double newBpm = 0.0, bar = 4.0, origin = 0.0;
    int offset = 0, ticket = 0;
    {
        const juce::ScopedLock lock (stateLock);
        if (take == nullptr)
            return;
        current = take;
        known = chroma; // the chroma does not depend on the tempo
        newBpm = manualBpm.load();
        bar = beatsPerBar;
        origin = originPpq;
        offset = analysisOffset;
        ticket = ++analysisTicket;
        bpm = newBpm;
        updatePlayback();
    }
    ++pendingAnalyses;
    worker.addJob ([this, current, known, newBpm, bar, origin, offset, ticket]
    {
        auto analysis = analyseTake (current, newBpm, bar, offset, origin, &known);
        post ([this, analysis, ticket]() mutable
        {
            {
                const juce::ScopedLock lock (stateLock);
                if (ticket == analysisTicket && analysis.take == take && analysis.ok())
                    installAnalysis (std::move (analysis), false, false);
            }
            --pendingAnalyses;
            ++revision;
        });
    });
}

void BacklineProcessor::setTransportPlaying (bool shouldPlay)
{
    if (shouldPlay && ! transportPlaying.load())
    {
        const juce::ScopedLock lock (stateLock);
        rewindTarget = originPpq; // play from the loop start
        rewindRequested = true;
    }
    transportPlaying = shouldPlay;
    ++revision;
}

void BacklineProcessor::setTakePlayback (bool shouldPlay)
{
    playTake = shouldPlay;
    ++revision;
}

//==============================================================================
// Instruments

void BacklineProcessor::setSoundsFolder (const juce::File& folder)
{
    const juce::ScopedLock lock (stateLock);
    soundsFolder = folder;
}

juce::String BacklineProcessor::getInstrument (Part part) const
{
    const juce::ScopedLock lock (stateLock);
    return requestedInstrument[partIndex (part)];
}

juce::String BacklineProcessor::getLoadedInstrument (Part part) const
{
    const juce::ScopedLock lock (stateLock);
    return loadedInstrument[partIndex (part)]->name;
}

bool BacklineProcessor::isInstrumentLoading (Part part) const
{
    if (part == Part::Melody)
        return false;
    const juce::ScopedLock lock (stateLock);
    return loadingInstrument[partIndex (part)].isNotEmpty();
}

void BacklineProcessor::installInstrument (int index, instruments::InstrumentPtr instrument)
{
    if (instrument == nullptr)
        instrument = instruments::builtIn();
    {
        const juce::ScopedLock lock (stateLock);
        if (loadedInstrument[index] == instrument)
            return; // already playing (instruments are shared while unchanged), so nothing to switch
    }
    auto synth = instruments::createSynth (instrument, index == 1, currentSampleRate.load());
    std::unique_ptr<juce::Synthesiser> old; // freed here, never on the audio thread
    {
        const juce::ScopedLock lock (stateLock);
        loadedInstrument[index] = std::move (instrument);
        const juce::SpinLock::ScopedLockType handoff (playbackLock);
        old = std::move (pendingSynth[index]);
        pendingSynth[index] = std::move (synth);
    }
    ++revision;
}

void BacklineProcessor::startInstrumentLoad (int index, const juce::String& name)
{
    juce::File root;
    int ticket = 0;
    {
        const juce::ScopedLock lock (stateLock);
        root = soundsFolder;
        ticket = ++instrumentTicket[index];
        loadingInstrument[index] = name;
    }
    ++pendingInstrumentLoads;
    ++revision; // the interface shows the part as loading
    worker.addJob ([this, index, name, root, ticket]
    {
        auto instrument = instruments::loadInstrument (root, name);
        post ([this, index, ticket, instrument]
        {
            bool current = false;
            {
                const juce::ScopedLock lock (stateLock);
                current = ticket == instrumentTicket[index];
            }
            if (current)
            {
                installInstrument (index, instrument); // nullptr (folder gone or empty) falls back to the synth
                const juce::ScopedLock lock (stateLock);
                if (ticket == instrumentTicket[index])
                    loadingInstrument[index].clear();
            }
            --pendingInstrumentLoads;
            ++revision;
        });
    });
}

void BacklineProcessor::setInstrument (Part part, const juce::String& name)
{
    if (part == Part::Melody)
        return;
    const int index = partIndex (part);
    const bool synth = name.isEmpty() || name.equalsIgnoreCase (instruments::builtInName);
    const juce::String wanted = synth ? juce::String (instruments::builtInName) : name;
    bool load = false;
    {
        const juce::ScopedLock lock (stateLock);
        requestedInstrument[index] = wanted;
        if (loadingInstrument[index] == wanted)
        {
            // already on its way
        }
        else if (synth || loadedInstrument[index]->name == wanted)
        {
            ++instrumentTicket[index]; // nothing to load: cancel a load of another one still running
            loadingInstrument[index].clear();
        }
        else
            load = true;
    }
    if (synth)
        installInstrument (index, instruments::builtIn()); // does nothing when the synth is already playing
    else if (load)
        startInstrumentLoad (index, wanted);
    ++revision;
}

juce::StringArray BacklineProcessor::getInstrumentNames()
{
    juce::File root;
    juce::String wanted[2], loaded[2];
    {
        const juce::ScopedLock lock (stateLock);
        root = soundsFolder;
        for (int p = 0; p < 2; ++p)
        {
            wanted[p] = requestedInstrument[p];
            loaded[p] = loadedInstrument[p]->name;
        }
    }
    const auto found = instruments::scanInstruments (root);
    for (int p = 0; p < 2; ++p)
    {
        if (wanted[p] == instruments::builtInName)
            continue;
        if (! found.contains (wanted[p]))
        {
            if (loaded[p] != instruments::builtInName)
                installInstrument (p, instruments::builtIn()); // the folder has gone
        }
        else if (loaded[p] != wanted[p] && pendingInstrumentLoads.load() == 0)
            startInstrumentLoad (p, wanted[p]);                // it is back (or was missing at restore)
    }
    juce::StringArray names { instruments::builtInName };
    names.addArray (found);
    return names;
}

//==============================================================================
// Export

juce::File BacklineProcessor::writeMidiFile (Part part)
{
    const auto notes = getPartNotes (part);
    if (notes.empty())
        return {};
    const auto bytes = backline::writeMidiFile (notes, getBpm());
    auto file = midiExportFolder (this).getChildFile (partName (part) + ".mid");
    return file.replaceWithData (bytes.data(), bytes.size()) ? file : juce::File();
}

juce::AudioBuffer<float> BacklineProcessor::renderPartAudio (Part part, double sampleRate) const
{
    if (part == Part::Melody || sampleRate <= 0.0)
        return {};
    std::vector<Note> notes;
    double loop = 0.0;
    instruments::InstrumentPtr instrument;
    {
        const juce::ScopedLock lock (stateLock);
        notes = getPartNotes (part);
        loop = progressionLength (progression);
        instrument = loadedInstrument[partIndex (part)];
    }
    if (notes.empty() || loop <= 0.0)
        return {};
    return instruments::renderNotes (notes, loop, getTempo(), sampleRate, instrument, part == Part::Bass, 4.0);
}

void BacklineProcessor::setExportFolder (const juce::File& folder)
{
    const juce::ScopedLock lock (stateLock);
    exportFolder = folder;
}

juce::File BacklineProcessor::writeWavFile (Part part)
{
    // Until the selected instrument has loaded, the synth would play instead of it.
    if (part == Part::Melody || isInstrumentLoading (part))
        return {};
    const double rate = currentSampleRate.load() >= 8000.0 ? currentSampleRate.load() : 48000.0;
    const auto audio = renderPartAudio (part, rate);
    if (audio.getNumSamples() == 0)
        return {};

    // The same audio as the last export of this part, and that file is still as it was written:
    // hand it out again rather than leave another copy.
    const int index = partIndex (part);
    const auto hash = hashOf (audio, rate);
    juce::File folder;
    {
        const juce::ScopedLock lock (stateLock);
        const auto& last = lastWav[index];
        if (last.hash == hash && last.file.existsAsFile() && last.file.getSize() == last.size
            && last.file.getLastModificationTime() == last.modified)
            return last.file;
        folder = exportFolder;
    }

    // A new file every time: a DAW may still refer to the earlier ones.
    if (! folder.createDirectory())
        folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("M1D1").getChildFile ("Exports");
    folder.createDirectory();
    const auto file = folder.getChildFile (partName (part) + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".wav")
                          .getNonexistentSibling();
    auto stream = file.createOutputStream();
    if (stream == nullptr)
        return {};
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), rate, 2u, 24, {}, 0));
    if (writer == nullptr)
        return {};
    stream.release(); // the writer owns it now
    const bool ok = writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    writer.reset();
    if (! ok)
    {
        file.deleteFile(); // never handed out
        return {};
    }
    const juce::ScopedLock lock (stateLock);
    lastWav[index] = { file, hash, file.getSize(), file.getLastModificationTime() };
    return file;
}

//==============================================================================
// State. Version 2 stores the chord events; the 0.1.x "progression" attribute (fixed grid,
// major/minor only) is still written for older versions and read when "events" is missing.

void BacklineProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement xml ("Backline");
    xml.setAttribute ("version", 2);
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
        juce::StringArray legacy, events;
        for (const auto& e : progression)
        {
            legacy.add (juce::String (e.chord.root) + (hasMinorThird (e.chord.quality) ? "m" : "M"));
            // root:quality:bass:start:length:flags (1 = 7th added by the Sevenths setting, 2 = spelled with a sharp)
            const int flags = (e.coloured ? 1 : 0) | (e.chord.sharpRoot ? 2 : 0);
            events.add (juce::String (e.chord.root) + ":" + juce::String ((int) e.chord.quality) + ":" + juce::String (e.chord.bass)
                        + ":" + juce::String (e.start, 9) + ":" + juce::String (e.length, 9) + ":" + juce::String (flags));
        }
        xml.setAttribute ("progression", legacy.joinIntoString (" "));
        xml.setAttribute ("events", events.joinIntoString (" "));
        xml.setAttribute ("sevenths", sevenths);
        xml.setAttribute ("chordInstrument", requestedInstrument[0]);
        xml.setAttribute ("bassInstrument", requestedInstrument[1]);
    }
    xml.setAttribute ("tempo", manualBpm.load());
    xml.setAttribute ("playTake", playTake.load());
    xml.setAttribute ("preview", previewEnabled.load());
    xml.setAttribute ("gain", (double) previewGain.load());
    {
        const juce::ScopedLock lock (webLock);
        if (webState.isNotEmpty()) xml.setAttribute ("webState", webState);
        if (webSongJson.isNotEmpty()) xml.setAttribute ("webSong", webSongJson);
    }
    copyXmlToBinary (xml, destData);
}

void BacklineProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("Backline"))
        return;
    {
        const juce::ScopedLock lock (webLock);
        webState = xml->getStringAttribute ("webState");
    }
    if (const auto song = xml->getStringAttribute ("webSong"); song.isNotEmpty())
        setWebSong (song);   // the parts play (and go out as MIDI) even before the window is opened
    previewEnabled = xml->getBoolAttribute ("preview", true);
    previewGain = (float) juce::jlimit (0.0, 1.0, xml->getDoubleAttribute ("gain", 0.7));
    manualBpm = juce::jlimit (minManualTempo, maxManualTempo, xml->getDoubleAttribute ("tempo", defaultTempo));
    playTake = xml->getBoolAttribute ("playTake", false);

    {
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
        sevenths = xml->getBoolAttribute ("sevenths", false);

        // The audio is not stored in the session, only the chosen progression.
        ++loadTicket;      // a file still loading belongs to the old session
        ++analysisTicket;
        take = nullptr;
        chroma = {};
        melodyNotes.clear();
        segments.clear();
        ideas.clear();
        estimatedBpm = 0.0;
        lastTakeTooShort = false;
        loadError.clear();

        Progression restored;
        for (const auto& token : juce::StringArray::fromTokens (xml->getStringAttribute ("events"), " ", ""))
        {
            const auto fields = juce::StringArray::fromTokens (token, ":", "");
            if (fields.size() < 5 || fields.size() > 6 || (int) restored.size() >= maxChords)
            {
                restored.clear();
                break;
            }
            ChordEvent e;
            e.chord.root = juce::jlimit (0, 11, fields[0].getIntValue());
            e.chord.quality = (Quality) juce::jlimit (0, numQualities - 1, fields[1].getIntValue());
            e.chord.bass = juce::jlimit (-1, 11, fields[2].getIntValue());
            e.start = fields[3].getDoubleValue();
            e.length = fields[4].getDoubleValue();
            const int flags = fields.size() > 5 ? fields[5].getIntValue() : 0; // not written by early 0.2 builds
            e.coloured = (flags & 1) != 0;
            e.chord.sharpRoot = (flags & 2) != 0;
            restored.push_back (e);
        }
        if (! isValidProgression (restored))
        {
            // A 0.1.x session: major/minor chords on a fixed grid.
            std::vector<Chord> chords;
            for (const auto& token : juce::StringArray::fromTokens (xml->getStringAttribute ("progression"), " ", ""))
                if (token.isNotEmpty() && (int) chords.size() < maxChords)
                    chords.push_back ({ juce::jlimit (0, 11, token.getIntValue()),
                                        token.endsWithChar ('m') ? Quality::Minor : Quality::Major });
            restored = chords.empty() ? Progression() : toProgression (chords, beatsPerBar * chordRate);
        }
        progression = std::move (restored);
        progressionEdited = true; // the user's chords: a later re-analysis of the same take keeps them
        updatePlayback();
    }

    setInstrument (Part::Chords, xml->getStringAttribute ("chordInstrument", instruments::builtInName));
    setInstrument (Part::Bass, xml->getStringAttribute ("bassInstrument", instruments::builtInName));
}

void BacklineProcessor::setWebSong (const juce::String& json)
{
    if (auto song = m1d1::parseSong (json))
    {
        {
            const juce::ScopedLock lock (webLock);
            webSongJson = json;
        }
        webEngine.setSong (std::move (song));
    }
}

juce::AudioProcessorEditor* BacklineProcessor::createEditor()
{
   #if M1D1_WEB_UI
    return new WebEditor (*this);
   #else
    return new BacklineEditor (*this);
   #endif
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BacklineProcessor(); }
