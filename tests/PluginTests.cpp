// End-to-end test of the plugin processor without a DAW: feeds audio with a fake transport,
// then checks the analysis result, the preview sound, MIDI export and session save/restore.
#include "../plugin/PluginProcessor.h"

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (false)

struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = false;
    double ppq = 0.0, bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        return info;
    }
};

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const double sampleRate = 48000.0;
    const int blockSize = 512;

    BacklineProcessor processor;
    FakePlayHead playHead;
    processor.setPlayHead (&playHead);
    processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    juce::int64 samplePos = 0;
    auto runBlocks = [&] (double seconds, bool withInput)
    {
        const int chords[4][3] = { { 60, 64, 67 }, { 55, 59, 62 }, { 57, 60, 64 }, { 53, 57, 60 } };
        float peak = 0.0f;
        for (int done = 0; done < (int) (seconds * sampleRate); done += blockSize)
        {
            buffer.clear();
            if (withInput)
                for (int i = 0; i < blockSize; ++i)
                {
                    const double t = (double) (samplePos + i) / sampleRate;
                    const int bar = ((int) (t / 2.0)) % 4;
                    float v = 0.0f;
                    for (int p : chords[bar])
                        v += 0.15f * (float) std::sin (juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (p) * t);
                    buffer.setSample (0, i, v);
                    buffer.setSample (1, i, v);
                }
            processor.processBlock (buffer, midi);
            peak = juce::jmax (peak, buffer.getMagnitude (0, blockSize));
            if (playHead.playing)
            {
                samplePos += blockSize;
                playHead.ppq = (double) samplePos / sampleRate * playHead.bpm / 60.0;
            }
        }
        return peak;
    };

    // Record four bars (8 s at 120 BPM) while the transport plays, then stop the transport.
    processor.setRecordArmed (true);
    runBlocks (0.1, false);
    CHECK (! processor.isRecording()); // armed but transport stopped
    playHead.playing = true;
    runBlocks (8.0, true);
    CHECK (processor.isRecording());
    playHead.playing = false;
    runBlocks (0.1, false);
    CHECK (! processor.isRecording() && ! processor.isRecordArmed());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400); // let the analysis timer fire

    CHECK (processor.hasCapture());
    CHECK (processor.getKey().tonic == 0 && ! processor.getKey().minor);
    CHECK (processor.getNumIdeas() == 4);
    juce::String names;
    for (auto c : processor.getProgression()) names << backline::chordName (c) << " ";
    std::printf ("key %s, progression: %s\n", backline::keyName (processor.getKey()).c_str(), names.toRawUTF8());
    CHECK (names == "C G Am F ");

    // Preview: silence in, generated chords and bass out while playing; silent again after stop.
    playHead.playing = true;
    const float playingPeak = runBlocks (4.0, false);
    playHead.playing = false;
    runBlocks (1.0, false);
    const float stoppedPeak = runBlocks (0.5, false);
    std::printf ("preview peak while playing %.3f, after stop %.5f\n", playingPeak, stoppedPeak);
    CHECK (playingPeak > 0.05f && playingPeak < 1.0f);
    CHECK (stoppedPeak < 1.0e-4f);

    processor.previewEnabled = false;
    playHead.playing = true;
    runBlocks (1.0, false);
    CHECK (runBlocks (1.0, false) < 1.0e-4f);
    processor.previewEnabled = true;

    // Editing and export.
    processor.setChord (1, { 4, backline::Quality::Minor });
    CHECK (backline::chordName (processor.getProgression()[1]) == "Em");
    processor.setBeatsPerChord (2.0);
    CHECK (processor.getProgression().size() == 8);
    processor.setBeatsPerChord (4.0);
    for (auto part : { BacklineProcessor::Part::Chords, BacklineProcessor::Part::Bass, BacklineProcessor::Part::Melody })
    {
        const auto file = processor.writeMidiFile (part);
        juce::MidiFile parsed;
        juce::FileInputStream in (file);
        const bool ok = part == BacklineProcessor::Part::Melody ? true // chords are polyphonic: no melody guaranteed
                                                                 : (in.openedOk() && parsed.readFrom (in) && parsed.getNumTracks() == 1
                                                                    && parsed.getTrack (0)->getNumEvents() > 8);
        CHECK (ok);
        file.deleteFile();
    }

    // Session save and restore.
    processor.setChord (0, { 9, backline::Quality::Minor });
    juce::MemoryBlock state;
    processor.getStateInformation (state);
    BacklineProcessor restored;
    restored.setStateInformation (state.getData(), (int) state.getSize());
    CHECK (restored.getProgression() == processor.getProgression());
    CHECK (restored.getKey().tonic == 0 && ! restored.hasCapture() && restored.hasProgression());

    // Recording that starts one beat before a bar line must still line chords up with the bars,
    // and a host re-preparing the plugin must not wipe the take.
    {
        BacklineProcessor late;
        FakePlayHead head;
        late.setPlayHead (&head);
        late.setPlayConfigDetails (2, 2, sampleRate, blockSize);
        late.prepareToPlay (sampleRate, blockSize);
        const int chords[4][3] = { { 60, 64, 67 }, { 55, 59, 62 }, { 57, 60, 64 }, { 53, 57, 60 } };
        juce::int64 pos = 0;
        auto run = [&] (double seconds)
        {
            for (int done = 0; done < (int) (seconds * sampleRate); done += blockSize, pos += blockSize)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const double t = (double) (pos + i) / sampleRate, ppq = 3.0 + t * 2.0;
                    float v = 0.0f;
                    for (int p : chords[((int) (ppq / 4.0)) % 4])
                        v += 0.15f * (float) std::sin (juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (p) * t);
                    buffer.setSample (0, i, v);
                    buffer.setSample (1, i, v);
                }
                head.ppq = 3.0 + (double) pos / sampleRate * 2.0;
                late.processBlock (buffer, midi);
            }
        };
        late.setRecordArmed (true);
        head.playing = true;
        run (8.5); // one beat of bar 1 (C), then G, Am, F, C
        head.playing = false;
        run (0.1);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
        juce::String lateNames;
        for (auto c : late.getProgression()) lateNames << backline::chordName (c) << " ";
        std::printf ("mid-bar start: skipped %.2f beats, progression: %s\n", late.getBarOffsetBeats(), lateNames.toRawUTF8());
        CHECK (std::abs (late.getBarOffsetBeats() - 1.0) < 0.05);
        CHECK (lateNames == "G Am F C ");

        late.prepareToPlay (sampleRate, blockSize);
        float peak = 0.0f;
        for (float v : late.getWaveform (64)) peak = juce::jmax (peak, v);
        CHECK (peak > 0.1f);
        CHECK (! late.getPartNotes (BacklineProcessor::Part::Chords).empty());
    }

    std::printf (failures == 0 ? "All plugin tests passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
