// End-to-end test of the plugin processor without a DAW: feeds audio with a fake transport,
// then checks the analysis result, the preview sound, MIDI export and session save/restore.
#include "../plugin/PluginProcessor.h"

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (false)

struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = false, musical = true;
    double ppq = 0.0, bpm = 120.0;
    int numerator = 4, denominator = 4;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        if (! musical) // like the standalone app: a playhead with a clock but no tempo or position
        {
            info.setTimeInSamples (0);
            return info;
        }
        info.setIsPlaying (playing);
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setTimeSignature (TimeSignature { numerator, denominator });
        return info;
    }
};

// Level of one frequency in a buffer (Goertzel), to tell which bass note is sounding.
static double toneLevel (const std::vector<float>& x, double frequency, double sampleRate)
{
    const double w = juce::MathConstants<double>::twoPi * frequency / sampleRate, c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (float v : x) { const double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; }
    return std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (double) std::max<size_t> (1, x.size());
}

// Drives a processor block by block with four-bar C G Am F audio laid out on the host timeline.
struct Rig
{
    BacklineProcessor processor;
    FakePlayHead head;
    double sampleRate = 48000.0, beatsPerChord = 4.0;
    int blockSize = 512;
    juce::int64 pos = 0;       // samples of transport time since the rig started
    double ppqOrigin = 0.0;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;

    Rig (double rate = 48000.0, int block = 512) : sampleRate (rate), blockSize (block), buffer (2, block)
    {
        processor.setPlayHead (&head);
        processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
    }

    // Returns the left output. withInput feeds the chord audio, otherwise silence.
    std::vector<float> run (double seconds, bool withInput)
    {
        static const int chords[4][3] = { { 60, 64, 67 }, { 55, 59, 62 }, { 57, 60, 64 }, { 53, 57, 60 } };
        std::vector<float> out;
        const double beatsPerSample = head.bpm / (60.0 * sampleRate);
        for (int done = 0; done < (int) (seconds * sampleRate); done += blockSize)
        {
            buffer.clear();
            head.ppq = ppqOrigin + (double) pos * beatsPerSample;
            if (withInput)
                for (int i = 0; i < blockSize; ++i)
                {
                    const double t = (double) (pos + i) / sampleRate, ppq = head.ppq + i * beatsPerSample;
                    float v = 0.0f;
                    for (int p : chords[((int) std::floor (ppq / beatsPerChord)) & 3])
                        v += 0.15f * (float) std::sin (juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (p) * t);
                    buffer.setSample (0, i, v);
                    buffer.setSample (1, i, v);
                }
            processor.processBlock (buffer, midi);
            out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
            if (head.playing || ! head.musical)
                pos += blockSize;
        }
        return out;
    }

    void analyse() { juce::MessageManager::getInstance()->runDispatchLoopUntil (400); }

    juce::String names()
    {
        juce::String s;
        for (auto c : processor.getProgression()) s << backline::chordName (c) << " ";
        return s;
    }

    // Records `seconds` of the chord audio from the current position and analyses it.
    void record (double seconds)
    {
        processor.setRecordArmed (true);
        head.playing = true;
        run (seconds, true);
        head.playing = false;
        run (0.1, false);
        analyse();
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
    processor.setChordRate (0.5);
    CHECK (processor.getProgression().size() == 8);
    processor.setChordRate (1.0);
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

    // No musical transport at all (the standalone app): record and preview on a free-running clock.
    {
        Rig rig;
        rig.head.musical = false;
        rig.processor.setRecordArmed (true);
        rig.run (8.0, true);
        CHECK (rig.processor.isRecording());
        rig.processor.setRecordArmed (false);
        rig.run (0.1, false);
        rig.analyse();
        std::printf ("no host transport: %s\n", rig.names().toRawUTF8());
        CHECK (rig.processor.hasCapture() && rig.processor.getProgression().size() == 4);
        float peak = 0.0f;
        for (float v : rig.run (2.0, false)) peak = juce::jmax (peak, std::abs (v));
        CHECK (peak > 0.05f);
        CHECK (rig.processor.getPlayheadBeats() >= 0.0);
    }

    // Seeking while playing must switch to the chord at the new position, with nothing left hanging.
    {
        Rig rig;
        rig.record (8.0);
        CHECK (rig.names() == "C G Am F ");
        rig.processor.setChordRhythm (backline::ChordRhythm::Sustain);
        rig.processor.setBassRhythm (backline::BassRhythm::Sustain);
        const double cBass = juce::MidiMessage::getMidiNoteInHertz (36), aBass = juce::MidiMessage::getMidiNoteInHertz (45);

        rig.pos = 0;
        rig.head.playing = true;
        rig.run (1.0, false);                                // beats 0-2, C
        rig.pos = (juce::int64) (4.5 * rig.sampleRate);      // jump to beat 9, in the Am bar
        rig.run (0.25, false);
        const auto afterSeek = rig.run (1.0, false);
        std::printf ("after seek: C bass %.4f, A bass %.4f\n", toneLevel (afterSeek, cBass, rig.sampleRate), toneLevel (afterSeek, aBass, rig.sampleRate));
        CHECK (toneLevel (afterSeek, aBass, rig.sampleRate) > 0.02);
        CHECK (toneLevel (afterSeek, cBass, rig.sampleRate) < 0.002);

        // Host bypass in the middle of a chord must not leave it droning afterwards.
        rig.pos = 0;
        rig.run (1.0, false);
        for (int i = 0; i < 50; ++i)
            rig.processor.processBlockBypassed (rig.buffer, rig.midi);
        rig.pos = (juce::int64) (4.5 * rig.sampleRate);
        rig.run (0.25, false);
        const auto afterBypass = rig.run (1.0, false);
        CHECK (toneLevel (afterBypass, cBass, rig.sampleRate) < 0.002);
        CHECK (toneLevel (afterBypass, aBass, rig.sampleRate) > 0.02);

        // Starting playback in the middle of a sustained chord must sound straight away.
        rig.head.playing = false;
        rig.run (1.0, false);
        rig.pos = (juce::int64) (0.5 * rig.sampleRate);      // beat 1, inside the C bar
        rig.head.playing = true;
        const auto midChord = rig.run (0.5, false);
        CHECK (toneLevel (midChord, cBass, rig.sampleRate) > 0.02);
    }

    // Events exactly on block boundaries are sent once: 48 kHz, 480-sample blocks, 100 BPM.
    {
        const std::vector<backline::Chord> prog { { 0, backline::Quality::Major }, { 7, backline::Quality::Major },
                                                  { 9, backline::Quality::Minor }, { 5, backline::Quality::Major } };
        const auto notes = backline::renderBass (prog, 4.0, backline::BassRhythm::Eighths);
        const double loop = 16.0, blockBeats = 480.0 * 100.0 / (60.0 * 48000.0);
        int ons = 0, offs = 0;
        double blockStart = 0.0;
        const int blocks = (int) std::llround (3.0 * loop / blockBeats);
        for (int i = 0; i < blocks; ++i)
        {
            const double hostBeat = std::fmod ((double) i * blockBeats, loop);
            const double blockEnd = std::fmod (hostBeat + blockBeats, loop);
            juce::MidiBuffer events;
            BacklineProcessor::scheduleNotes (notes, events, blockStart, blockEnd, loop, 480);
            for (const auto meta : events)
                (meta.getMessage().isNoteOn() ? ons : offs)++;
            blockStart = blockEnd;
        }
        std::printf ("block-boundary scheduling: %d note-ons, %d note-offs for %d notes x 3 passes\n", ons, offs, (int) notes.size());
        CHECK (ons == 3 * (int) notes.size());
        CHECK (offs == 3 * (int) notes.size());
    }

    // A host loop wrap while recording ends the take there, so the audio stays linear on the timeline.
    {
        Rig rig;
        rig.processor.setRecordArmed (true);
        rig.head.playing = true;
        rig.run (4.0, true);            // bars 1-2: C G
        rig.pos = 0;                    // the host cycles back to bar 1
        rig.run (1.0, true);
        CHECK (! rig.processor.isRecording() && ! rig.processor.isRecordArmed());
        rig.head.playing = false;
        rig.run (0.1, false);
        rig.analyse();
        std::printf ("take cut at loop wrap: %s\n", rig.names().toRawUTF8());
        CHECK (rig.names() == "C G ");
    }

    // 3/4 time: one chord per three-beat bar.
    {
        Rig rig;
        rig.head.numerator = 3;
        rig.beatsPerChord = 3.0;
        rig.record (12.0);              // 24 beats = 8 bars
        std::printf ("3/4 take: %s(%.0f beats per bar)\n", rig.names().toRawUTF8(), rig.processor.getBeatsPerBar());
        CHECK (rig.names() == "C G Am F C G Am F ");
        CHECK (rig.processor.getLoopBeats() == 24.0);
    }

    // A blip of a take after a good one must leave a consistent session.
    {
        Rig rig;
        rig.record (8.0);
        rig.pos = 0;
        rig.record (0.2);
        CHECK (rig.processor.wasLastTakeTooShort());
        CHECK (! rig.processor.hasCapture() && rig.processor.getProgression().size() == 4);
        rig.processor.setChordRate (0.5);
        CHECK (rig.processor.getLoopBeats() == (double) rig.processor.getPartNotes (BacklineProcessor::Part::Chords).back().start
                                                   + rig.processor.getPartNotes (BacklineProcessor::Part::Chords).back().length);
        rig.pos = 0;
        rig.record (8.0);
        CHECK (! rig.processor.wasLastTakeTooShort() && rig.processor.hasCapture());
    }

    // A sample-rate change after a take keeps the take; the next take uses the new rate.
    {
        Rig rig;
        rig.record (8.0);
        rig.processor.prepareToPlay (44100.0, 512);
        CHECK (std::abs (rig.processor.getRecordedSeconds() - 8.0) < 0.1);
        float peak = 0.0f;
        for (float v : rig.processor.getWaveform (64)) peak = juce::jmax (peak, v);
        CHECK (peak > 0.1f);
        rig.sampleRate = 44100.0;
        rig.pos = 0;
        rig.record (8.0);
        CHECK (rig.names() == "C G Am F ");
        CHECK (std::abs (rig.processor.getRecordedSeconds() - 8.0) < 0.1);
    }

    std::printf (failures == 0 ? "All plugin tests passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
