// End-to-end test of the plugin processor without a DAW: feeds audio with a fake transport,
// then checks the analysis result, the preview sound, MIDI export and session save/restore.
#include "../plugin/PluginEditor.h"

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
        for (auto c : processor.getChords()) s << backline::chordName (c) << " ";
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


//==============================================================================
// Helpers for the 0.2 tests: audio files, waiting for background work, measurements.

static bool waitFor (const std::function<bool()>& done, int timeoutMs = 20000)
{
    for (int waited = 0; waited < timeoutMs; waited += 20)
    {
        if (done())
            return true;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }
    return done();
}

static juce::File testFolder()
{
    static const juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                         .getChildFile ("backline-plugin-tests-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();
    return folder;
}

static juce::File writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
{
    file.deleteFile();
    auto stream = file.createOutputStream();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, (unsigned int) audio.getNumChannels(), 24, {}, 0));
    if (writer == nullptr)
        return {};
    stream.release();
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

// C G Am F, one bar (four beats) each, repeated for `seconds`. Plucked chords restart on every
// beat with a decay, so the tempo can be heard; otherwise they are held.
static juce::AudioBuffer<float> chordAudio (double seconds, double sampleRate, double bpm, bool plucked, float amplitude)
{
    static const int chords[4][3] = { { 60, 64, 67 }, { 55, 59, 62 }, { 57, 60, 64 }, { 53, 57, 60 } };
    juce::AudioBuffer<float> audio (2, (int) (seconds * sampleRate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const double t = i / sampleRate, beat = t * bpm / 60.0, inBeat = (beat - std::floor (beat)) * 60.0 / bpm;
        const double env = plucked ? std::exp (-inBeat * 7.0) * juce::jmin (1.0, inBeat * 500.0) : 1.0;
        float v = 0.0f;
        for (int p : chords[((int) std::floor (beat / 4.0)) & 3])
            v += amplitude * (float) (env * std::sin (juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (p) * t));
        audio.setSample (0, i, v);
        audio.setSample (1, i, v);
    }
    return audio;
}

static std::vector<float> slice (const std::vector<float>& x, double fromSeconds, double toSeconds, double sampleRate)
{
    const auto a = (size_t) juce::jlimit (0.0, (double) x.size(), fromSeconds * sampleRate);
    const auto b = (size_t) juce::jlimit ((double) a, (double) x.size(), toSeconds * sampleRate);
    return std::vector<float> (x.begin() + (std::ptrdiff_t) a, x.begin() + (std::ptrdiff_t) b);
}

static float largestStep (const std::vector<float>& x, size_t from, size_t to)
{
    float step = 0.0f;
    for (size_t i = juce::jmax<size_t> (1, from); i < juce::jmin (to, x.size()); ++i)
        step = juce::jmax (step, std::abs (x[i] - x[i - 1]));
    return step;
}

static std::vector<float> channel (const juce::AudioBuffer<float>& audio, int ch)
{
    return std::vector<float> (audio.getReadPointer (ch), audio.getReadPointer (ch) + audio.getNumSamples());
}

// A restored session holding C G Am F, a bar each, in C major (as a 0.1.1 host would hand it over).
static void loadLegacySession (BacklineProcessor& p, const juce::String& chords, int tonic = 0, bool minor = false,
                               double chordRate = 1.0)
{
    juce::XmlElement xml ("Backline");
    xml.setAttribute ("tonic", tonic);
    xml.setAttribute ("minor", minor);
    xml.setAttribute ("bpm", 120.0);
    xml.setAttribute ("origin", 0.0);
    xml.setAttribute ("beatsPerBar", 4.0);
    xml.setAttribute ("chordRate", chordRate);
    xml.setAttribute ("chordRhythm", 0);
    xml.setAttribute ("bassRhythm", 0);
    xml.setAttribute ("progression", chords);
    xml.setAttribute ("preview", true);
    xml.setAttribute ("gain", 0.7);
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (xml, block);
    p.setStateInformation (block.getData(), (int) block.getSize());
}

static juce::String chordNames (const BacklineProcessor& p)
{
    juce::String s;
    for (auto c : p.getChords()) s << backline::chordName (c) << " ";
    return s;
}

static void versionTwoTests();
static void editorTests();

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
    for (auto c : processor.getChords()) names << backline::chordName (c) << " ";
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
    CHECK (backline::chordName (processor.getChords()[1]) == "Em");
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
        for (auto c : late.getChords()) lateNames << backline::chordName (c) << " ";
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

    versionTwoTests();
    editorTests();
    testFolder().deleteRecursively();

    std::printf (failures == 0 ? "All plugin tests passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

//==============================================================================
// 0.2: audio files, own transport, take playback, chord editing, state, WAV export, instruments.
static void versionTwoTests()
{
    using Part = BacklineProcessor::Part;
    const auto folder = testFolder();

    // A WAV file as the take, in a DAW (host transport at 120 BPM): the right chords, and the
    // take plays back by default because it came from a file.
    {
        Rig rig;
        rig.run (0.05, false); // the processor learns there is a host transport
        const auto file = writeWav (folder.getChildFile ("chords.wav"), chordAudio (8.0, 48000.0, 120.0, false, 0.15f), 48000.0);
        rig.processor.loadAudioFile (file);
        CHECK (rig.processor.isLoadingFile());
        CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
        std::printf ("file in a DAW: %s(%s, %.1f beats)\n", chordNames (rig.processor).toRawUTF8(),
                     rig.processor.getTakeName().toRawUTF8(), rig.processor.getTakeBeats());
        CHECK (rig.processor.hasHostTransport());
        CHECK (rig.processor.getLoadError().isEmpty());
        CHECK (chordNames (rig.processor) == "C G Am F ");
        CHECK (rig.processor.getTakeName() == "chords.wav" && rig.processor.isTakeFromFile());
        CHECK (rig.processor.getKey().tonic == 0 && ! rig.processor.getKey().minor);
        CHECK (rig.processor.isTakePlaybackOn());
        CHECK (std::abs (rig.processor.getTakeBeats() - 16.0) < 0.05);
        CHECK (std::abs (rig.processor.getRecordedSeconds() - 8.0) < 0.01);

        // The manual tempo is inactive with a host transport.
        rig.processor.setManualTempo (60.0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        CHECK (! rig.processor.isAnalysing() && std::abs (rig.processor.getTakeBeats() - 16.0) < 0.05);
        CHECK (std::abs (rig.processor.getTempo() - 120.0) < 1.0e-9);

        // Files that cannot be used leave the session alone.
        rig.processor.loadAudioFile (folder.getChildFile ("missing.wav"));
        CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
        CHECK (rig.processor.getLoadError().isNotEmpty() && chordNames (rig.processor) == "C G Am F ");
        const auto junk = folder.getChildFile ("junk.wav");
        junk.replaceWithText ("not audio");
        rig.processor.loadAudioFile (junk);
        CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
        std::printf ("unusable file: \"%s\"\n", rig.processor.getLoadError().toRawUTF8());
        CHECK (rig.processor.getLoadError().isNotEmpty() && rig.processor.getTakeName() == "chords.wav");
        CHECK (BacklineProcessor::isAudioFile (file) && ! BacklineProcessor::isAudioFile (folder.getChildFile ("a.txt")));

        // Takes recorded from a DAW track do not play back by default (the track already does).
        rig.processor.setTakePlayback (true);
        rig.record (8.0);
        CHECK (rig.processor.getTakeName() == "Recorded take" && ! rig.processor.isTakeFromFile());
        CHECK (! rig.processor.isTakePlaybackOn());
    }

    // The standalone app (no host transport): a 44.1 kHz file of plucked chords at 100 BPM gets its
    // tempo estimated and applied; changing the tempo re-analyses; play and stop work.
    {
        Rig rig;
        rig.head.musical = false;
        rig.run (0.05, false);
        CHECK (! rig.processor.hasHostTransport());
        const auto file = writeWav (folder.getChildFile ("plucked.wav"), chordAudio (19.2, 44100.0, 100.0, true, 0.15f), 44100.0);
        rig.processor.loadAudioFile (file);
        CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
        std::printf ("standalone file: estimate %.2f BPM, tempo %.2f, %s\n", rig.processor.getEstimatedTempo(),
                     rig.processor.getTempo(), chordNames (rig.processor).toRawUTF8());
        CHECK (std::abs (rig.processor.getEstimatedTempo() - 100.0) < 0.5);
        CHECK (std::abs (rig.processor.getManualTempo() - rig.processor.getEstimatedTempo()) < 1.0e-9);
        CHECK (std::abs (rig.processor.getBpm() - rig.processor.getEstimatedTempo()) < 1.0e-9);
        CHECK (chordNames (rig.processor) == "C G Am F C G Am F ");
        CHECK (rig.processor.isTakePlaybackOn());

        // Play / stop of the own transport.
        rig.processor.previewEnabled = true;
        const auto playing = rig.run (1.0, false);
        float peak = 0.0f;
        for (float v : playing) peak = juce::jmax (peak, std::abs (v));
        CHECK (peak > 0.05f && rig.processor.getPlayheadBeats() >= 0.0);
        rig.processor.setTransportPlaying (false);
        rig.run (1.0, false); // release tails
        float stopped = 0.0f;
        for (float v : rig.run (0.5, false)) stopped = juce::jmax (stopped, std::abs (v));
        std::printf ("own transport: peak %.3f playing, %.6f stopped\n", peak, stopped);
        CHECK (stopped < 1.0e-4f && rig.processor.getPlayheadBeats() < 0.0 && ! rig.processor.isTransportPlaying());
        rig.processor.setTransportPlaying (true);
        rig.run (0.02, false);
        CHECK (rig.processor.getPlayheadBeats() >= 0.0 && rig.processor.getPlayheadBeats() < 0.1); // from the loop start

        // A new tempo re-analyses the take: 19.2 s at 50 BPM is 16 beats.
        rig.processor.setManualTempo (50.0);
        CHECK (waitFor ([&] { return ! rig.processor.isAnalysing(); }));
        std::printf ("at 50 BPM: %.2f beats, %s\n", rig.processor.getTakeBeats(), chordNames (rig.processor).toRawUTF8());
        CHECK (std::abs (rig.processor.getTakeBeats() - 16.0) < 0.05 && std::abs (rig.processor.getBpm() - 50.0) < 1.0e-9);
        CHECK (rig.processor.getChords().size() == 4 && std::abs (rig.processor.getLoopBeats() - 16.0) < 1.0e-9);
        rig.processor.setManualTempo (1000.0);
        CHECK (std::abs (rig.processor.getManualTempo() - BacklineProcessor::maxManualTempo) < 1.0e-9);
        CHECK (waitFor ([&] { return ! rig.processor.isAnalysing(); }));

        // Recording without a host transport plays the take back by default.
        rig.processor.setManualTempo (120.0);
        CHECK (waitFor ([&] { return ! rig.processor.isAnalysing(); }));
        rig.processor.setRecordArmed (true);
        rig.run (8.0, true);
        rig.processor.setRecordArmed (false);
        rig.run (0.1, false);
        rig.analyse();
        CHECK (rig.processor.getTakeName() == "Recorded take" && rig.processor.isTakePlaybackOn());
    }

    // Committing the tempo the take is analysed at (Return in the tempo field, a click elsewhere)
    // changes nothing. A real tempo change re-analyses but keeps the chords the user has edited.
    {
        Rig rig;
        rig.head.musical = false;
        rig.run (0.05, false);
        rig.processor.loadAudioFile (writeWav (folder.getChildFile ("edits.wav"), chordAudio (19.2, 44100.0, 100.0, true, 0.15f), 44100.0));
        CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
        CHECK (rig.processor.getNumIdeas() > 1);
        rig.processor.selectIdea (1);
        CHECK (rig.processor.splitChord (0));
        CHECK (rig.processor.setChord (2, { 2, backline::Quality::Minor7 }));
        const auto edited = rig.processor.getProgression();
        const double tempo = rig.processor.getManualTempo();
        const int revision = rig.processor.getRevision();

        rig.processor.setManualTempo (tempo);
        CHECK (! rig.processor.isAnalysing() && rig.processor.getRevision() == revision);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        CHECK (rig.processor.getProgression() == edited && rig.processor.getSelectedIdea() == 1);

        rig.processor.setManualTempo (tempo * 1.25);
        CHECK (rig.processor.isAnalysing());
        CHECK (waitFor ([&] { return ! rig.processor.isAnalysing(); }));
        std::printf ("after a tempo change: %s(%.2f BPM, %.1f beats in the take)\n", chordNames (rig.processor).toRawUTF8(),
                     rig.processor.getBpm(), rig.processor.getTakeBeats());
        CHECK (std::abs (rig.processor.getBpm() - tempo * 1.25) < 1.0e-9 && std::abs (rig.processor.getTakeBeats() - 40.0) < 0.1);
        CHECK (rig.processor.getProgression() == edited && rig.processor.getSelectedIdea() == 1);
        CHECK (rig.processor.getNumIdeas() > 1 && rig.processor.hasCapture());

        // Picking an idea puts fresh chords in place, and those follow the next tempo change.
        rig.processor.selectIdea (0);
        const auto idea = rig.processor.getProgression();
        rig.processor.setManualTempo (tempo);
        CHECK (waitFor ([&] { return ! rig.processor.isAnalysing(); }));
        CHECK (rig.processor.getProgression() != idea && std::abs (rig.processor.getLoopBeats() - 32.0) < 1.0e-9);
        CHECK (rig.processor.getSelectedIdea() == 0);
    }

    // A passing dominant (or any 7th chord picked by hand) survives turning Sevenths on and off,
    // also across a session save and restore; so does the sharp spelling of a passing chord.
    {
        BacklineProcessor p;
        loadLegacySession (p, "0M 5M");
        CHECK (p.insertPassingChord (0) && chordNames (p) == "C C7 F ");
        p.setSevenths (true);
        CHECK (chordNames (p) == "Cmaj7 C7 Fmaj7 ");
        p.setSevenths (false);
        CHECK (chordNames (p) == "C C7 F ");
        p.setSevenths (true);
        CHECK (p.setChord (2, { 9, backline::Quality::Minor7 })); // picked from the menu while Sevenths is on
        juce::MemoryBlock state;
        p.getStateInformation (state);
        BacklineProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (restored.getSevenths() && chordNames (restored) == "Cmaj7 C7 Am7 ");
        restored.setSevenths (false);
        CHECK (chordNames (restored) == "C C7 Am7 ");

        BacklineProcessor inF;
        loadLegacySession (inF, "5M 7m", 5);
        CHECK (inF.insertPassingChord (0));
        CHECK (backline::chordName (inF.getChords()[1], inF.getKey()) == "F#dim");
        inF.getStateInformation (state);
        restored.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (backline::chordName (restored.getChords()[1], restored.getKey()) == "F#dim");
    }

    // Take playback is locked to the loop: an impulse at beat 5 of a 44.1 kHz file comes out exactly
    // at beat 5 of the 48 kHz output, in every pass, and the loop point does not click.
    {
        Rig rig;
        rig.head.musical = false;
        rig.run (0.05, false);
        auto audio = chordAudio (8.0, 44100.0, 120.0, false, 0.05f);
        for (int ch = 0; ch < 2; ++ch)
            audio.setSample (ch, 110250, 0.9f); // beat 5 at 120 BPM = 2.5 s
        rig.processor.setManualTempo (120.0);
        rig.processor.loadAudioFile (writeWav (folder.getChildFile ("impulse.wav"), audio, 44100.0));
        CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
        CHECK (chordNames (rig.processor) == "C G Am F " && std::abs (rig.processor.getTempo() - 120.0) < 1.0e-9);
        rig.processor.previewEnabled = false;
        const auto out = rig.run (12.0, false); // starts at the loop start (a new file rewinds)
        auto peakNear = [&] (double seconds)
        {
            size_t best = 0;
            for (size_t i = (size_t) ((seconds - 0.1) * 48000.0); i < (size_t) ((seconds + 0.1) * 48000.0); ++i)
                if (std::abs (out[i]) > std::abs (out[best])) best = i;
            return best;
        };
        const auto first = peakNear (2.5), second = peakNear (10.5);
        std::printf ("take playback: impulse at %d and %d (expected 120000 and 504000), %.3f\n", (int) first, (int) second, out[first]);
        CHECK (std::abs ((int) first - 120000) <= 1 && std::abs ((int) second - 504000) <= 1);
        CHECK (out[first] > 0.6f);
        const float loopStep = largestStep (out, 384000 - 400, 384000 + 400), normalStep = largestStep (out, 300000, 300800);
        std::printf ("largest step at the loop point %.4f (elsewhere %.4f)\n", loopStep, normalStep);
        CHECK (loopStep < 0.02f);

        // Turning take playback off fades out without a click.
        rig.processor.setTakePlayback (false);
        const auto off = rig.run (0.5, false);
        CHECK (largestStep (off, 0, off.size()) < 0.02f);
        float tail = 0.0f;
        for (size_t i = 2000; i < off.size(); ++i) tail = juce::jmax (tail, std::abs (off[i]));
        CHECK (tail < 1.0e-5f);
    }

    // Seeking in a DAW while the take plays: the audio follows to the new bar, cleanly.
    {
        Rig rig;
        rig.run (0.05, false);
        juce::AudioBuffer<float> audio (1, 8 * 48000);
        const double tones[4] = { 300.0, 400.0, 500.0, 600.0 }; // one per bar
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (0, i, 0.2f * (float) std::sin (juce::MathConstants<double>::twoPi * tones[i / 96000] * i / 48000.0));
        rig.processor.loadAudioFile (writeWav (folder.getChildFile ("tones.wav"), audio, 48000.0));
        CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
        CHECK (rig.processor.isTakePlaybackOn());
        rig.processor.previewEnabled = false;
        rig.head.playing = true;
        rig.pos = 0;
        const auto before = rig.run (1.0, false);
        CHECK (toneLevel (before, 300.0, 48000.0) > 0.05);
        rig.pos = (juce::int64) (4.5 * 48000.0); // jump to beat 9, in bar 3
        const auto seek = rig.run (0.25, false);
        const auto after = rig.run (1.0, false);
        std::printf ("take after seek: 500 Hz %.4f, 300 Hz %.4f, largest step %.4f\n", toneLevel (after, 500.0, 48000.0),
                     toneLevel (after, 300.0, 48000.0), largestStep (seek, 0, seek.size()));
        CHECK (toneLevel (after, 500.0, 48000.0) > 0.05 && toneLevel (after, 300.0, 48000.0) < 0.002);
        CHECK (largestStep (seek, 0, seek.size()) < 0.05f);
        rig.head.playing = false; // stop: fades out
        const auto stopped = rig.run (0.5, false);
        CHECK (largestStep (stopped, 0, stopped.size()) < 0.05f);
        CHECK (std::abs (stopped.back()) < 1.0e-6f);
    }

    // Chord editing through the processor on a restored session.
    {
        BacklineProcessor p;
        loadLegacySession (p, "0M 7M 9m 5M");
        CHECK (chordNames (p) == "C G Am F " && std::abs (p.getLoopBeats() - 16.0) < 1.0e-9);
        CHECK (std::abs (p.getGridBeats() - 1.0) < 1.0e-9);
        const int revision = p.getRevision();

        CHECK (p.splitChord (0));
        auto prog = p.getProgression();
        CHECK (prog.size() == 5 && std::abs (prog[0].length - 2.0) < 1.0e-9 && std::abs (prog[1].start - 2.0) < 1.0e-9);
        CHECK (p.getRevision() != revision);
        CHECK (p.getPartNotes (Part::Chords).size() == 15); // five sustained triads

        CHECK (p.moveChordBoundary (0, 3.2)); // snaps to beat 3
        prog = p.getProgression();
        CHECK (std::abs (prog[0].length - 3.0) < 1.0e-9 && std::abs (prog[1].start - 3.0) < 1.0e-9 && std::abs (prog[1].length - 1.0) < 1.0e-9);
        CHECK (! p.moveChordBoundary (0, 3.9)); // the next chord would be shorter than a beat

        CHECK (p.deleteChord (1));
        prog = p.getProgression();
        CHECK (prog.size() == 4 && std::abs (prog[0].length - 4.0) < 1.0e-9 && chordNames (p) == "C G Am F ");

        CHECK (p.insertPassingChord (0));
        prog = p.getProgression();
        const auto passing = backline::passingChord ({ 0, backline::Quality::Major }, { 7, backline::Quality::Major }, p.getKey());
        std::printf ("passing chord from C to G: %s\n", backline::chordName (prog[1].chord).c_str());
        CHECK (prog.size() == 5 && prog[1].chord == passing && std::abs (prog[1].start - 3.0) < 1.0e-9);
        CHECK (std::abs (prog[1].length - 1.0) < 1.0e-9 && std::abs (p.getLoopBeats() - 16.0) < 1.0e-9);
        CHECK (p.getChordIndexAt (3.5) == 1 && p.getChordIndexAt (16.0) == -1);

        const auto variations = p.getVariations (0);
        CHECK (! variations.empty());
        for (auto v : variations) CHECK (v != prog[0].chord);
        CHECK (p.setChord (0, { 0, backline::Quality::Major, 4 })); // C/E
        CHECK (backline::chordName (p.getChords()[0]) == "C/E");
        const auto bass = p.getPartNotes (Part::Bass);
        CHECK (! bass.empty() && bass.front().pitch % 12 == 4);
        CHECK (! p.setChord (0, { 0, backline::Quality::Major, 4 })); // no change
        CHECK (! p.setChord (9, { 2, backline::Quality::Minor }) && ! p.deleteChord (-1) && ! p.splitChord (17));
        CHECK (p.getAlternatives (0).size() >= 6);

        // Sevenths on and off.
        p.setChord (0, { 0, backline::Quality::Major });
        p.deleteChord (1);
        p.setSevenths (true);
        std::printf ("with sevenths: %s\n", chordNames (p).toRawUTF8());
        CHECK (p.getSevenths() && chordNames (p) == "Cmaj7 G7 Am7 Fmaj7 ");
        for (auto c : p.getAlternatives (0)) CHECK (c.quality != backline::Quality::Major || c.root != 0);
        CHECK (p.getPartNotes (Part::Chords).front().pitch > 0 && p.getPartNotes (Part::Chords).size() == 16);
        p.setSevenths (false);
        CHECK (chordNames (p) == "C G Am F ");

        // Never below one chord, never above 64.
        while (p.getChords().size() > 1)
            CHECK (p.deleteChord (0));
        CHECK (! p.deleteChord (0) && std::abs (p.getLoopBeats() - 16.0) < 1.0e-9);
        juce::StringArray many;
        for (int i = 0; i < BacklineProcessor::maxChords; ++i) many.add ("0M");
        loadLegacySession (p, many.joinIntoString (" "));
        CHECK ((int) p.getChords().size() == BacklineProcessor::maxChords);
        CHECK (! p.splitChord (0) && ! p.insertPassingChord (0));
    }

    // Session state: a 0.1.1 blob still loads, and the new state survives a round trip.
    {
        BacklineProcessor old;
        old.setSoundsFolder (folder.getChildFile ("NoSounds"));
        loadLegacySession (old, "9m 5M 0M 7M", 9, true, 2.0);
        const auto prog = old.getProgression();
        std::printf ("0.1.1 session: %s\n", chordNames (old).toRawUTF8());
        CHECK (chordNames (old) == "Am F C G " && prog.size() == 4);
        CHECK (std::abs (prog[3].start - 24.0) < 1.0e-9 && std::abs (prog[3].length - 8.0) < 1.0e-9);
        CHECK (old.getKey().tonic == 9 && old.getKey().minor && ! old.getSevenths());
        CHECK (old.getInstrument (Part::Chords) == instruments::builtInName && ! old.hasCapture());

        old.splitChord (1);
        old.setChord (0, { 9, backline::Quality::Minor7, 0 });
        old.setSevenths (true);
        old.setManualTempo (97.0);
        old.setTakePlayback (true);
        old.setChordRhythm (backline::ChordRhythm::Offbeat);
        old.setInstrument (Part::Bass, "Upright");   // not installed: saved anyway, the synth plays
        CHECK (waitFor ([&] { return ! old.isInstrumentLoading(); }));
        CHECK (old.getLoadedInstrument (Part::Bass) == instruments::builtInName);
        juce::MemoryBlock state;
        old.getStateInformation (state);

        BacklineProcessor restored;
        restored.setSoundsFolder (folder.getChildFile ("NoSounds"));
        restored.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (waitFor ([&] { return ! restored.isInstrumentLoading(); }));
        std::printf ("round trip: %s\n", chordNames (restored).toRawUTF8());
        CHECK (restored.getProgression() == old.getProgression());
        CHECK (restored.getSevenths() && restored.isTakePlaybackOn());
        CHECK (std::abs (restored.getManualTempo() - 97.0) < 1.0e-9);
        CHECK (restored.getChordRhythm() == backline::ChordRhythm::Offbeat);
        CHECK (restored.getInstrument (Part::Bass) == "Upright" && restored.getLoadedInstrument (Part::Bass) == instruments::builtInName);
        CHECK (restored.getKey().tonic == 9 && restored.getKey().minor);

        // The old format is still written alongside, so 0.1.x can read a 0.2 session.
        const auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        CHECK (xml != nullptr && xml->getStringAttribute ("progression").startsWith ("9m"));
    }

    // WAV export: one loop plus the release tail, 24-bit stereo, with the bass on the right notes.
    {
        Rig rig;
        rig.run (0.05, false);
        rig.processor.setExportFolder (folder.getChildFile ("Exports"));
        loadLegacySession (rig.processor, "0M 7M 9m 5M");
        rig.processor.setBassRhythm (backline::BassRhythm::Sustain);
        const auto file = rig.processor.writeWavFile (Part::Bass);
        CHECK (file.getParentDirectory() == folder.getChildFile ("Exports") && file.getFileName().startsWith ("Backline Bass "));
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        CHECK (reader != nullptr);
        if (reader != nullptr)
        {
            const int loopSamples = 16 * 24000; // 16 beats at 120 BPM, 48 kHz
            std::printf ("bass WAV: %d samples, %d bits, %d channels, %.0f Hz\n", (int) reader->lengthInSamples,
                         (int) reader->bitsPerSample, (int) reader->numChannels, reader->sampleRate);
            CHECK (reader->bitsPerSample == 24 && reader->numChannels == 2 && std::abs (reader->sampleRate - 48000.0) < 1.0e-9);
            CHECK (reader->lengthInSamples >= loopSamples && reader->lengthInSamples < loopSamples + 48000);
            juce::AudioBuffer<float> audio (2, (int) reader->lengthInSamples);
            reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
            const auto left = channel (audio, 0);
            const double c = juce::MidiMessage::getMidiNoteInHertz (36), a = juce::MidiMessage::getMidiNoteInHertz (45);
            const auto bar1 = slice (left, 0.1, 1.9, 48000.0), bar3 = slice (left, 4.1, 5.9, 48000.0);
            std::printf ("bar 1: C %.4f A %.4f; bar 3: C %.4f A %.4f\n", toneLevel (bar1, c, 48000.0), toneLevel (bar1, a, 48000.0),
                         toneLevel (bar3, c, 48000.0), toneLevel (bar3, a, 48000.0));
            CHECK (toneLevel (bar1, c, 48000.0) > 0.03 && toneLevel (bar1, a, 48000.0) < 0.003);
            CHECK (toneLevel (bar3, a, 48000.0) > 0.03 && toneLevel (bar3, c, 48000.0) < 0.003);
        }
        const auto chords = rig.processor.renderPartAudio (Part::Chords, 44100.0);
        CHECK (chords.getNumSamples() >= 16 * 22050 && chords.getMagnitude (0, chords.getNumSamples()) > 0.05f);
        CHECK (rig.processor.writeWavFile (Part::Melody) == juce::File());

        // A DAW keeps referring to a dropped WAV, so a later export never overwrites it: a changed
        // part goes to a new file and the first stays as it was. The same audio again is the same file.
        const auto firstSize = file.getSize();
        const auto firstTime = file.getLastModificationTime();
        CHECK (rig.processor.writeWavFile (Part::Bass) == file);
        CHECK (rig.processor.setChord (0, { 2, backline::Quality::Minor }));
        const auto second = rig.processor.writeWavFile (Part::Bass);
        std::printf ("WAV exports: %s, %s\n", file.getFileName().toRawUTF8(), second.getFileName().toRawUTF8());
        CHECK (second.existsAsFile() && second != file && file.existsAsFile());
        CHECK (file.getSize() == firstSize && file.getLastModificationTime() == firstTime);
        CHECK (rig.processor.writeWavFile (Part::Bass) == second);
        CHECK (rig.processor.setChord (0, { 0, backline::Quality::Major }));
        const auto third = rig.processor.writeWavFile (Part::Bass); // the first audio again, but that file may be in use
        CHECK (third.existsAsFile() && third != file && third != second && third.getSize() == firstSize);
        const auto chordsFile = rig.processor.writeWavFile (Part::Chords);
        CHECK (chordsFile.getFileName().startsWith ("Backline Chords ") && chordsFile != third);
        for (const auto& f : { file, second, third, chordsFile })
            f.deleteFile();
    }

    // Sample instruments from a folder of generated sine samples.
    {
        CHECK (instruments::noteFromFileName ("Piano C4") == 60 && instruments::noteFromFileName ("Bb2") == 46);
        CHECK (instruments::noteFromFileName ("bass_F#2") == 42 && instruments::noteFromFileName ("Cs4") == 61);
        CHECK (instruments::noteFromFileName ("E-Piano A3") == 57 && instruments::noteFromFileName ("C-1") == 0);
        CHECK (instruments::noteFromFileName ("Grand 060 v2") == 60 && instruments::noteFromFileName ("48") == 48);
        CHECK (instruments::noteFromFileName ("Ambience") == -1 && instruments::noteFromFileName ("Db3") == 49);

        const auto sounds = folder.getChildFile ("Sounds");
        const auto sines = sounds.getChildFile ("Sines");
        auto makeSamples = [&]
        {
            sines.createDirectory();
            const std::pair<const char*, int> files[] = { { "Sine C2.wav", 36 }, { "Sine_C3.wav", 48 }, { "sine-c4.wav", 60 }, { "72.wav", 72 } };
            for (const auto& f : files)
            {
                juce::AudioBuffer<float> audio (1, 3 * 44100);
                const double hz = juce::MidiMessage::getMidiNoteInHertz (f.second);
                for (int i = 0; i < audio.getNumSamples(); ++i)
                    audio.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / 44100.0));
                writeWav (sines.getChildFile (f.first), audio, 44100.0);
            }
            sines.getChildFile ("readme.txt").replaceWithText ("not a sample");
        };
        makeSamples();
        sounds.getChildFile ("Empty").createDirectory();

        Rig rig;
        rig.run (0.05, false);
        loadLegacySession (rig.processor, "0M 7M 9m 5M");
        rig.processor.setBassRhythm (backline::BassRhythm::Sustain);
        rig.processor.setSoundsFolder (sounds);
        const auto names = rig.processor.getInstrumentNames();
        std::printf ("instruments: %s\n", names.joinIntoString (", ").toRawUTF8());
        CHECK (names.size() == 2 && names[0] == instruments::builtInName && names[1] == "Sines");
        rig.processor.setInstrument (Part::Bass, "Sines");
        CHECK (waitFor ([&] { return ! rig.processor.isInstrumentLoading(); }));
        CHECK (rig.processor.getLoadedInstrument (Part::Bass) == "Sines" && rig.processor.getInstrument (Part::Bass) == "Sines");
        CHECK (rig.processor.getLoadedInstrument (Part::Chords) == instruments::builtInName);
        {
            // Decoded once while in use: a second load of the unchanged folder shares the samples.
            const auto a = instruments::loadInstrument (sounds, "Sines"), b = instruments::loadInstrument (sounds, "Sines");
            CHECK (a != nullptr && a == b && a->zones.size() == 4 && a->zones[3].rootNote == 72);
            CHECK (instruments::loadInstrument (sounds, "Empty") == nullptr && instruments::loadInstrument (sounds, "../Sounds") == nullptr);
        }

        // Each bar's bass note at its own pitch: C from the C2 sample, G and A shifted down from C3, F up from C2.
        auto checkPitches = [&] (const std::vector<float>& x, double offsetSeconds, const char* what)
        {
            const int notes[4] = { 36, 43, 45, 41 };
            for (int bar = 0; bar < 4; ++bar)
            {
                const auto part = slice (x, offsetSeconds + bar * 2.0 + 0.2, offsetSeconds + bar * 2.0 + 1.8, 48000.0);
                const double hz = juce::MidiMessage::getMidiNoteInHertz (notes[bar]);
                const double right = toneLevel (part, hz, 48000.0);
                const double wrong = juce::jmax (toneLevel (part, hz * std::pow (2.0, 1.0 / 12.0), 48000.0),
                                                 toneLevel (part, hz / std::pow (2.0, 1.0 / 12.0), 48000.0),
                                                 toneLevel (part, hz * 2.0, 48000.0));
                std::printf ("%s bar %d: %.1f Hz %.4f, neighbours %.5f\n", what, bar + 1, hz, right, wrong);
                CHECK (right > 0.02 && wrong < 0.1 * right);
            }
        };
        const auto rendered = rig.processor.renderPartAudio (Part::Bass, 48000.0);
        checkPitches (channel (rendered, 0), 0.0, "sampled bass (export)");

        // The live preview plays the same instrument.
        rig.processor.previewEnabled = true;
        rig.head.playing = true;
        rig.pos = 0;
        rig.processor.setChordRhythm (backline::ChordRhythm::Sustain);
        rig.processor.previewGain = 1.0f;
        const auto live = rig.run (8.0, false);
        // The chord synth plays too, but in a higher range; check just the bass notes.
        checkPitches (live, 0.0, "sampled bass (live)");
        rig.head.playing = false;
        rig.run (0.5, false);

        // While an instrument loads, its part offers no WAV (the synth would play it instead).
        rig.processor.setExportFolder (folder.getChildFile ("Exports"));
        rig.processor.setInstrument (Part::Chords, "Sines");
        CHECK (rig.processor.isInstrumentLoading (Part::Chords) && ! rig.processor.isInstrumentLoading (Part::Bass));
        CHECK (rig.processor.writeWavFile (Part::Chords) == juce::File());
        const auto bassWav = rig.processor.writeWavFile (Part::Bass);
        CHECK (bassWav.existsAsFile());
        CHECK (waitFor ([&] { return ! rig.processor.isInstrumentLoading(); }));
        CHECK (! rig.processor.isInstrumentLoading (Part::Chords) && rig.processor.getLoadedInstrument (Part::Chords) == "Sines");
        const auto chordsWav = rig.processor.writeWavFile (Part::Chords);
        CHECK (chordsWav.existsAsFile());
        bassWav.deleteFile();
        chordsWav.deleteFile();
        // Asking for what is already playing loads nothing.
        rig.processor.setInstrument (Part::Chords, "Sines");
        CHECK (! rig.processor.isInstrumentLoading());
        rig.processor.setInstrument (Part::Chords, instruments::builtInName);
        CHECK (rig.processor.getLoadedInstrument (Part::Chords) == instruments::builtInName);

        // Changing the instrument while notes sound crossfades: no step in the output. Sustained
        // bass notes from the sampled sines, then the synth takes over in the middle of a note.
        rig.processor.setChordRhythm (backline::ChordRhythm::Sustain);
        rig.head.playing = true;
        rig.pos = 0;
        rig.run (1.0, false);
        const auto steady = rig.run (0.2, false);
        rig.processor.setInstrument (Part::Bass, instruments::builtInName);
        const auto change = rig.run (0.2, false);
        const float normal = largestStep (steady, 0, steady.size()), atChange = largestStep (change, 0, change.size());
        std::printf ("instrument change while playing: largest step %.4f (before %.4f)\n", atChange, normal);
        CHECK (atChange < 2.0f * normal + 0.002f);
        // The same instrument again (a session restore does that) changes nothing.
        juce::MemoryBlock session;
        rig.processor.getStateInformation (session);
        rig.processor.setStateInformation (session.getData(), (int) session.getSize());
        CHECK (! rig.processor.isInstrumentLoading());
        rig.processor.setInstrument (Part::Bass, "Sines");
        CHECK (waitFor ([&] { return ! rig.processor.isInstrumentLoading(); }));
        rig.head.playing = false;
        rig.run (0.5, false);

        // The folder disappears: the next rescan falls back to the synth, keeping the choice.
        sines.deleteRecursively();
        CHECK (! rig.processor.getInstrumentNames().contains ("Sines"));
        CHECK (rig.processor.getLoadedInstrument (Part::Bass) == instruments::builtInName);
        CHECK (rig.processor.getInstrument (Part::Bass) == "Sines");
        const auto synthBass = rig.processor.renderPartAudio (Part::Bass, 48000.0);
        CHECK (synthBass.getNumSamples() > 0 && synthBass.getMagnitude (0, synthBass.getNumSamples()) > 0.05f);
        // ...and comes back when the folder does.
        makeSamples();
        CHECK (rig.processor.getInstrumentNames().contains ("Sines"));
        CHECK (waitFor ([&] { return ! rig.processor.isInstrumentLoading(); }));
        CHECK (rig.processor.getLoadedInstrument (Part::Bass) == "Sines");

        // A session naming an instrument whose folder is missing loads with the synth.
        rig.processor.setInstrument (Part::Chords, "Gone");
        CHECK (waitFor ([&] { return ! rig.processor.isInstrumentLoading(); }));
        CHECK (rig.processor.getLoadedInstrument (Part::Chords) == instruments::builtInName);
        rig.processor.setInstrument (Part::Bass, instruments::builtInName);
        CHECK (rig.processor.getLoadedInstrument (Part::Bass) == instruments::builtInName);
    }
}

//==============================================================================
// 0.2 interface: chord card layout, the card menu, boundary dragging, the own transport's
// controls, file drops, the export chips and the instrument menu.

static juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> position, juce::Point<float> downAt, bool dragged)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, &c, &c, now, downAt, now, 1, dragged);
}

template <typename Type>
static std::vector<Type*> childrenOfType (juce::Component& parent)
{
    std::vector<Type*> found;
    for (auto* c : parent.getChildren())
        if (auto* t = dynamic_cast<Type*> (c))
            found.push_back (t);
    return found;
}

static void editorTests()
{
    using backline::Chord;
    using backline::Quality;
    using Part = BacklineProcessor::Part;

    // Roman numerals for every chord type.
    {
        const backline::Key c { 0, false, 1.0f };
        CHECK (ui::romanNumeral ({ 7, Quality::Dominant7 }, c) == "V7");
        CHECK (ui::romanNumeral ({ 9, Quality::Minor7 }, c) == "vi7");
        CHECK (ui::romanNumeral ({ 5, Quality::Major7 }, c) == "IVmaj7");
        CHECK (ui::romanNumeral ({ 2, Quality::Minor }, c) == "ii");
        CHECK (ui::romanNumeral ({ 7, Quality::Sus4 }, c) == "Vsus4");
        CHECK (ui::romanNumeral ({ 0, Quality::Major, 4 }, c) == "I");
        CHECK (ui::romanNumeral ({ 11, Quality::Diminished }, c) == "vii" + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0")));
        CHECK (ui::romanNumeral ({ 10, Quality::Major }, c) == "bVII");
    }

    // Card layout: widths follow the lengths; long progressions wrap onto rows of whole bars.
    {
        const auto eight = backline::toProgression ({ { 0 }, { 5 }, { 7 }, { 0 }, { 9, Quality::Minor }, { 5 }, { 7 }, { 0 } }, 4.0);
        auto l = ui::StripLayout::make (eight, 4.0, 800.0f, 160.0f);
        CHECK (l.rows == 1 && std::abs (l.beatsPerRow - 32.0) < 1.0e-9);
        auto p = l.pieces (4.0, 4.0);
        CHECK (p.size() == 1 && std::abs (p[0].getX() - 100.0f) < 0.01f && std::abs (p[0].getWidth() - 100.0f) < 0.01f);
        CHECK (l.boundaryAt (eight, { 101.0f, 80.0f }, 5.0f) == 0);
        CHECK (l.boundaryAt (eight, { 150.0f, 80.0f }, 5.0f) == -1);
        CHECK (std::abs (l.beatAt ({ 150.0f, 10.0f }) - 6.0) < 1.0e-9);

        // A passing chord does not reflow the strip: the typical chord decides.
        auto withPassing = backline::insertPassingChord (eight, 3, { 0, false, 1.0f });
        CHECK (withPassing.size() == 9 && ui::StripLayout::make (withPassing, 4.0, 800.0f, 160.0f).rows == 1);

        // 32 one-beat chords: two rows of four bars, 50 px each.
        std::vector<Chord> many ((size_t) 32, Chord { 0 });
        const auto dense = backline::toProgression (many, 1.0);
        l = ui::StripLayout::make (dense, 4.0, 800.0f, 160.0f);
        CHECK (l.rows == 2 && std::abs (l.beatsPerRow - 16.0) < 1.0e-9);
        p = l.pieces (16.0, 1.0);
        CHECK (p.size() == 1 && std::abs (p[0].getY() - 80.0f) < 0.01f && std::abs (p[0].getX()) < 0.01f);
        CHECK (std::abs (l.beatAt ({ 0.0f, 100.0f }) - 16.0) < 1.0e-9);
        // The boundary on the row break can be taken at the end of row 1 or the start of row 2.
        CHECK (l.boundaryAt (dense, { 799.0f, 40.0f }, 5.0f) == 15);
        CHECK (l.boundaryAt (dense, { 1.0f, 120.0f }, 5.0f) == 15);

        // A chord across a row break is drawn in two pieces.
        std::vector<Chord> sixteen ((size_t) 16, Chord { 0 });
        const auto mixed = backline::moveBoundary (backline::toProgression (sixteen, 2.0), 7, 17.0, 1.0); // chord 7: beats 14..17
        CHECK (std::abs (mixed[7].start - 14.0) < 1.0e-9 && std::abs (mixed[7].length - 3.0) < 1.0e-9);
        l = ui::StripLayout::make (mixed, 4.0, 640.0f, 160.0f); // 2 beats = 40 px on one row: too narrow
        CHECK (l.rows == 2);
        p = l.pieces (mixed[7].start, mixed[7].length);
        CHECK (p.size() == 2 && p[0].getRight() > 639.0f && std::abs (p[1].getX()) < 0.01f && p[1].getY() > 79.0f);
        // Empty: nothing to lay out, and no division by zero either.
        l = ui::StripLayout::make ({}, 4.0, 800.0f, 160.0f);
        CHECK (l.rows == 1 && std::abs (l.beatAt ({ 400.0f, 80.0f })) < 1.0e-9);

        // A one-beat passing chord among long ones is a few pixels wide; its middle is still the
        // card (it opens its menu), and its edges can still be dragged.
        std::vector<Chord> sixteenBars ((size_t) 16, Chord { 0 });
        for (size_t i = 0; i < sixteenBars.size(); ++i) sixteenBars[i].root = (int) (i * 7) % 12;
        for (double beats : { 8.0, 4.0 })
        {
            const auto narrow = backline::insertPassingChord (backline::toProgression (sixteenBars, beats), 3, { 0, false, 1.0f });
            l = ui::StripLayout::make (narrow, 4.0, 852.0f, 164.0f);
            p = l.pieces (narrow[4].start, narrow[4].length);
            CHECK (narrow[4].length == 1.0 && p.size() == 1);
            const auto centre = p[0].getCentre();
            std::printf ("passing card at %.0f beats a chord: %.1f px wide\n", beats, p[0].getWidth());
            CHECK (l.boundaryAt (narrow, centre, 5.0f) < 0);
            for (size_t i = 0; i < narrow.size(); ++i)
                for (const auto& piece : l.pieces (narrow[i].start, narrow[i].length))
                    CHECK (piece.contains (centre) == (i == 4));
            CHECK (l.boundaryAt (narrow, { p[0].getX(), centre.y }, 5.0f) == 3);
            CHECK (l.boundaryAt (narrow, { p[0].getRight(), centre.y }, 5.0f) == 4);
            CHECK (l.boundaryAt (narrow, { p[0].getX() - 4.0f, centre.y }, 5.0f) == 3); // from the wide card's side: the full 5 px
        }
    }

    // What the bass note of a slash chord is called in the menu.
    {
        CHECK (ui::bassNoteLabel ({ 0, Quality::Major, 4 }) == "Third in the bass");
        CHECK (ui::bassNoteLabel ({ 0, Quality::Major, 7 }) == "Fifth in the bass");
        CHECK (ui::bassNoteLabel ({ 0, Quality::Sus4, 5 }) == "Fourth in the bass");
        CHECK (ui::bassNoteLabel ({ 0, Quality::Sus2, 2 }) == "Second in the bass");
        CHECK (ui::bassNoteLabel ({ 9, Quality::Minor, 0 }) == "Third in the bass");
        CHECK (ui::bassNoteLabel ({ 11, Quality::Diminished, 5 }) == "Fifth in the bass");
        CHECK (ui::bassNoteLabel ({ 9, Quality::Minor7, 7 }) == "Seventh in the bass");
        CHECK (ui::bassNoteLabel ({ 2, Quality::Major }) == "Root in the bass");
        CHECK (ui::bassNoteLabel ({ 0, Quality::Major, 1 }).isEmpty());
    }

    // The editor in a DAW, after a take: no own transport controls; the card menu and its edits;
    // dragging a boundary; the export chips.
    {
        Rig rig;
        rig.record (16.0);
        CHECK (rig.processor.getProgression().size() == 4 || rig.processor.getProgression().size() == 8);
        std::unique_ptr<juce::AudioProcessorEditor> editorHolder (rig.processor.createEditor());
        auto& editor = *dynamic_cast<BacklineEditor*> (editorHolder.get());
        editor.refresh();
        for (auto* t : childrenOfType<ui::TempoField> (editor))
            CHECK (! t->isVisible());
        for (auto* t : childrenOfType<ui::TransportButton> (editor))
            CHECK (! t->isVisible());

        auto& strip = editor.getChordStrip();
        strip.setBounds (0, 0, 800, 160);
        const auto before = rig.processor.getProgression();
        const int n = (int) before.size();

        auto menu = strip.buildMenu (0);
        CHECK (! menu.choices.empty());
        bool split = false, passing = false, del = false, typeMenu = false;
        for (juce::PopupMenu::MenuItemIterator it (menu.menu); it.next();)
        {
            const auto& item = it.getItem();
            split = split || (item.itemID == ui::ChordStrip::splitId && item.isEnabled);
            passing = passing || (item.itemID == ui::ChordStrip::passingId && item.isEnabled && item.shortcutKeyDescription.isNotEmpty());
            del = del || (item.itemID == ui::ChordStrip::deleteId && item.isEnabled);
            typeMenu = typeMenu || (item.text == "Chord type" && item.subMenu != nullptr && item.subMenu->getNumItems() > 0);
        }
        CHECK (split && passing && del && typeMenu);

        // Each menu action does what it says.
        CHECK (ui::ChordStrip::applyMenuResult (rig.processor, 0, ui::ChordStrip::splitId, menu.choices));
        CHECK ((int) rig.processor.getProgression().size() == n + 1);
        CHECK (ui::ChordStrip::applyMenuResult (rig.processor, 0, ui::ChordStrip::deleteId, menu.choices));
        CHECK ((int) rig.processor.getProgression().size() == n);
        CHECK (ui::ChordStrip::applyMenuResult (rig.processor, 1, ui::ChordStrip::passingId, menu.choices));
        CHECK ((int) rig.processor.getProgression().size() == n + 1);
        CHECK (std::abs (rig.processor.getLoopBeats() - backline::progressionLength (before)) < 1.0e-9);
        rig.processor.deleteChord (2);
        // Picking a variation from the "Chord type" list changes the chord to it.
        menu = strip.buildMenu (0);
        const auto variations = rig.processor.getVariations (0);
        CHECK (! variations.empty());
        for (size_t i = 0; i < menu.choices.size(); ++i)
            if (menu.choices[i] == variations.front())
            {
                CHECK (ui::ChordStrip::applyMenuResult (rig.processor, 0, (int) i + 1, menu.choices));
                CHECK (rig.processor.getChords().front() == variations.front());
                break;
            }
        // The last chord cannot be deleted: the item is greyed out.
        while (rig.processor.getProgression().size() > 1)
            rig.processor.deleteChord (0);
        menu = strip.buildMenu (0);
        for (juce::PopupMenu::MenuItemIterator it (menu.menu); it.next();)
            if (it.getItem().itemID == ui::ChordStrip::deleteId)
                CHECK (! it.getItem().isEnabled);
        rig.processor.selectIdea (0);

        // Dragging the boundary after the first chord: it follows the mouse, snapped to beats.
        const auto start = rig.processor.getProgression();
        auto l = strip.layout();
        const double firstEnd = start[0].start + start[0].length;
        const auto line = l.boundaryLine (firstEnd);
        const juce::Point<float> down (line.getStartX() + 2.0f, line.getStartY() + 40.0f);
        CHECK (strip.chordAt (down) == -1 || l.boundaryAt (start, down, 5.0f) == 0);
        strip.mouseDown (mouseAt (strip, down, down, false));
        const double target = firstEnd + 1.6; // snaps to firstEnd + 2
        const float targetX = (float) ((target - std::floor (target / l.beatsPerRow) * l.beatsPerRow) / l.beatsPerRow) * l.width;
        strip.mouseDrag (mouseAt (strip, { targetX, down.y }, down, true));
        auto now = rig.processor.getProgression();
        CHECK (std::abs (now[0].length - (start[0].length + 2.0)) < 1.0e-9);
        CHECK (std::abs (now[1].length - (start[1].length - 2.0)) < 1.0e-9);
        // ...and can be dragged back in the same gesture.
        strip.mouseDrag (mouseAt (strip, { line.getStartX() - (float) (l.width / l.beatsPerRow), down.y }, down, true));
        strip.mouseUp (mouseAt (strip, down, down, true));
        now = rig.processor.getProgression();
        CHECK (std::abs (now[0].length - (start[0].length - 1.0)) < 1.0e-9);
        CHECK (now.size() == start.size());
        // A click on a card is not a drag: nothing moves.
        CHECK (strip.chordAt ({ 20.0f, 80.0f }) == 0);

        // The progression is replaced while a menu is open or a boundary is being dragged (a new
        // take, a tempo change): the menu's choice and the drag no longer apply to anything.
        rig.processor.selectIdea (0);
        const auto shown = rig.processor.getProgression();
        menu = strip.buildMenu (0);
        rig.processor.selectIdea (1);
        const auto replaced = rig.processor.getProgression();
        CHECK (replaced != shown);
        CHECK (! ui::ChordStrip::applyMenuResult (rig.processor, shown, 0, ui::ChordStrip::deleteId, menu.choices));
        CHECK (! ui::ChordStrip::applyMenuResult (rig.processor, shown, 0, 1, menu.choices));
        CHECK (rig.processor.getProgression() == replaced);
        CHECK (ui::ChordStrip::applyMenuResult (rig.processor, replaced, 0, ui::ChordStrip::splitId, menu.choices)); // still current: applied
        rig.processor.selectIdea (0);
        l = strip.layout();
        const auto grab = l.boundaryLine (shown[0].start + shown[0].length);
        const juce::Point<float> at (grab.getStartX(), grab.getStartY() + 40.0f);
        strip.mouseDown (mouseAt (strip, at, at, false));
        rig.processor.selectIdea (1);
        strip.mouseDrag (mouseAt (strip, at.translated ((float) (l.width / l.beatsPerRow) * 2.0f, 0.0f), at, true));
        strip.mouseDrag (mouseAt (strip, at.translated ((float) (l.width / l.beatsPerRow) * 3.0f, 0.0f), at, true));
        strip.mouseUp (mouseAt (strip, at, at, true));
        CHECK (rig.processor.getProgression() == replaced);
        rig.processor.selectIdea (0);

        // The tiles: Chords and Bass offer MIDI and WAV, Audio to MIDI only MIDI.
        auto tiles = childrenOfType<ui::PartTile> (editor);
        CHECK (tiles.size() == 3);
        if (tiles.size() == 3)
        {
            CHECK (! tiles[0]->chipBounds (ui::PartTile::midi).isEmpty() && ! tiles[0]->chipBounds (ui::PartTile::wav).isEmpty());
            CHECK (! tiles[1]->chipBounds (ui::PartTile::wav).isEmpty());
            CHECK (tiles[2]->chipBounds (ui::PartTile::wav).isEmpty());
            const auto wavChip = tiles[0]->chipBounds (ui::PartTile::wav);
            CHECK (tiles[0]->chipAt (wavChip.getCentre()) == ui::PartTile::wav);
            CHECK (! tiles[0]->chipBounds (ui::PartTile::midi).intersects (wavChip));
            CHECK (! wavChip.intersects (tiles[0]->instrument.getBounds()));
            // The same 6 px between the WAV chip and the instrument as between the two chips.
            CHECK (wavChip.getX() - tiles[0]->chipBounds (ui::PartTile::midi).getRight() == 6);
            CHECK (tiles[0]->instrument.getX() - wavChip.getRight() == 6);
            CHECK (tiles[0]->instrument.getRight() == tiles[0]->getWidth() - 14);
        }

        // Audio files dropped on the window become the take; other files are ignored.
        const auto folder = testFolder().getChildFile ("ui");
        folder.createDirectory();
        CHECK (editor.isInterestedInFileDrag ({ folder.getChildFile ("song.wav").getFullPathName() }));
        CHECK (editor.isInterestedInFileDrag ({ "/x/notes.txt", folder.getChildFile ("song.FLAC").getFullPathName() }));
        CHECK (! editor.isInterestedInFileDrag ({ "/x/notes.txt", "/x/project.als" }));
        const auto song = writeWav (folder.getChildFile ("Dropped song.wav"), chordAudio (8.0, 48000.0, 120.0, false, 0.15f), 48000.0);
        editor.fileDragEnter ({ song.getFullPathName() }, 10, 10);
        editor.filesDropped ({ "/x/notes.txt", song.getFullPathName() }, 10, 10);
        CHECK (rig.processor.isLoadingFile());
        CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
        CHECK (rig.processor.getTakeName() == "Dropped song.wav" && rig.processor.isTakeFromFile());

        // The instrument menu lists the synth and the folders found when it opens.
        const auto sounds = folder.getChildFile ("Sounds");
        rig.processor.setSoundsFolder (sounds);
        if (tiles.size() == 3)
        {
            auto& box = tiles[0]->instrument;
            tiles[0]->updateInstruments (true);
            CHECK (box.getNumItems() == 2 && box.getItemText (0) == instruments::builtInName && box.getSelectedId() == 1);
            juce::AudioBuffer<float> tone (1, 44100);
            for (int i = 0; i < tone.getNumSamples(); ++i)
                tone.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 261.63 * i / 44100.0));
            sounds.getChildFile ("Keys").createDirectory();
            writeWav (sounds.getChildFile ("Keys").getChildFile ("Keys C4.wav"), tone, 44100.0);
            tiles[0]->updateInstruments (true); // as when the menu opens: the new folder appears
            CHECK (box.getNumItems() == 3 && box.getItemText (1) == "Keys");
            box.setSelectedId (2, juce::sendNotificationSync);
            CHECK (rig.processor.getInstrument (Part::Chords) == "Keys");
            CHECK (waitFor ([&] { return ! rig.processor.isInstrumentLoading(); }));
            CHECK (rig.processor.getLoadedInstrument (Part::Chords) == "Keys");
            editor.refresh();
            CHECK (box.getText() == "Keys");
            CHECK (tiles[1]->instrument.getText() == instruments::builtInName);
        }
    }

    // The standalone app (no host transport): play/stop and the tempo field are shown, and
    // dragging the tempo field sets the manual tempo when the mouse is released.
    {
        Rig rig;
        rig.head.musical = false;
        rig.run (0.05, false);
        CHECK (! rig.processor.hasHostTransport());
        std::unique_ptr<juce::AudioProcessorEditor> editorHolder (rig.processor.createEditor());
        auto& editor = *dynamic_cast<BacklineEditor*> (editorHolder.get());
        editor.refresh();
        auto fields = childrenOfType<ui::TempoField> (editor);
        auto buttons = childrenOfType<ui::TransportButton> (editor);
        CHECK (fields.size() == 1 && buttons.size() == 1);
        if (fields.size() == 1 && buttons.size() == 1)
        {
            auto& field = *fields.front();
            CHECK (field.isVisible() && buttons.front()->isVisible());
            CHECK (std::abs (field.getShownTempo() - BacklineProcessor::defaultTempo) < 1.0e-9);
            const juce::Point<float> down (40.0f, 17.0f);
            field.mouseDown (mouseAt (field, down, down, false));
            field.mouseDrag (mouseAt (field, down.translated (0.0f, -20.0f), down, true)); // up: faster
            CHECK (std::abs (field.getShownTempo() - 130.0) < 1.0e-9);
            CHECK (std::abs (rig.processor.getManualTempo() - 120.0) < 1.0e-9); // not until released
            field.mouseUp (mouseAt (field, down.translated (0.0f, -20.0f), down, true));
            CHECK (std::abs (rig.processor.getManualTempo() - 130.0) < 1.0e-9);
            // Far past the range: clamped.
            field.mouseDown (mouseAt (field, down, down, false));
            field.mouseDrag (mouseAt (field, down.translated (0.0f, 2000.0f), down, true));
            field.mouseUp (mouseAt (field, down.translated (0.0f, 2000.0f), down, true));
            CHECK (std::abs (rig.processor.getManualTempo() - BacklineProcessor::minManualTempo) < 1.0e-9);
            CHECK (ui::TempoField::format (96.0) == "96" && ui::TempoField::format (96.5) == "96.5");

            // Play / stop toggles the own transport.
            const bool wasPlaying = rig.processor.isTransportPlaying();
            buttons.front()->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            CHECK (rig.processor.isTransportPlaying() != wasPlaying);

            // A take, edited chords, and a tempo that is not a whole number (97.43, shown as 97.4).
            const auto folder = testFolder().getChildFile ("tempo-ui");
            folder.createDirectory();
            rig.processor.loadAudioFile (writeWav (folder.getChildFile ("take.wav"), chordAudio (19.2, 44100.0, 100.0, true, 0.15f), 44100.0));
            CHECK (waitFor ([&] { return ! rig.processor.isLoadingFile(); }));
            rig.processor.setManualTempo (97.43);
            CHECK (waitFor ([&] { return ! rig.processor.isAnalysing(); }));
            rig.processor.selectIdea (1);
            CHECK (rig.processor.splitChord (1) && rig.processor.setChord (0, { 4, Quality::Minor7 }));
            const auto edited = rig.processor.getProgression();
            editor.refresh();
            CHECK (ui::TempoField::format (field.getShownTempo()) == "97.4");
            auto unchanged = [&]
            {
                return ! rig.processor.isAnalysing() && std::abs (rig.processor.getManualTempo() - 97.43) < 1.0e-9
                    && rig.processor.getProgression() == edited && rig.processor.getSelectedIdea() == 1;
            };

            // Click to type, then Return (or a click elsewhere) without changing the value.
            field.showEditor();
            auto typing = childrenOfType<juce::TextEditor> (field);
            CHECK (typing.size() == 1 && typing.front()->getText() == "97.4");
            if (typing.size() == 1)
                typing.front()->onReturnKey();
            CHECK (unchanged());
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100); // the text editor goes
            field.showEditor();
            typing = childrenOfType<juce::TextEditor> (field);
            if (typing.size() == 1)
                typing.front()->onFocusLost();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            CHECK (unchanged());
            // A drag that ends where it began.
            field.mouseDown (mouseAt (field, down, down, false));
            field.mouseDrag (mouseAt (field, down.translated (0.0f, -20.0f), down, true));
            field.mouseDrag (mouseAt (field, down, down, true));
            field.mouseUp (mouseAt (field, down, down, true));
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            CHECK (unchanged());
            // The wheel turned one step up and one back.
            juce::MouseWheelDetails wheel {};
            wheel.deltaY = 0.2f;
            field.mouseWheelMove (mouseAt (field, down, down, false), wheel);
            CHECK (ui::TempoField::format (field.getShownTempo()) == "98");
            wheel.deltaY = -0.2f;
            field.mouseWheelMove (mouseAt (field, down, down, false), wheel);
            CHECK (ui::TempoField::format (field.getShownTempo()) == "97.4");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (700);
            CHECK (! field.isEditing() && unchanged());

            // A real change still applies, and the edited chords stay.
            field.showEditor();
            typing = childrenOfType<juce::TextEditor> (field);
            if (typing.size() == 1)
            {
                typing.front()->setText ("112", false);
                typing.front()->onReturnKey();
            }
            CHECK (std::abs (rig.processor.getManualTempo() - 112.0) < 1.0e-9 && rig.processor.isAnalysing());
            CHECK (waitFor ([&] { return ! rig.processor.isAnalysing(); }));
            CHECK (rig.processor.getProgression() == edited && std::abs (rig.processor.getBpm() - 112.0) < 1.0e-9);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        }
        // Once a host transport appears, they are hidden.
        rig.head.musical = true;
        rig.run (0.05, false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        CHECK (rig.processor.hasHostTransport());
        if (fields.size() == 1)
            CHECK (! fields.front()->isVisible());
    }
}
