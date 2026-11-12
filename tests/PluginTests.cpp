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

    std::printf (failures == 0 ? "All plugin tests passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
