#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Backline.h"
#include "Instruments.h"

// Threading: every public function except processBlock / processBlockBypassed may be called from
// the message thread (and the state functions from any host thread); they take stateLock and never
// block the audio thread. Slow work (decoding files, re-analysing a take, loading sample
// instruments) runs on a background worker and is installed by a timer on the message thread, so
// its results appear a little later: watch getRevision() or the isLoadingFile / isAnalysing /
// isInstrumentLoading flags. The audio thread receives new data only as immutable objects handed
// over with a try-lock, and everything it drops is freed on the message thread.
class BacklineProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    BacklineProcessor();
    ~BacklineProcessor() override;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { haltPreviewAndTake(); }
    void reset() override { haltPreviewAndTake(); }
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "M1D1"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }

    // MIDI out to the host: chords on channel 1, bass on channel 2.
    static constexpr int chordMidiChannel = 1, bassMidiChannel = 2;
    void setMidiOutEnabled (bool on) { midiOutEnabled.store (on); }
    bool isMidiOutEnabled() const { return midiOutEnabled.load(); }
    double getTailLengthSeconds() const override { return 0.5; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==========================================================================
    // Recording from the track. Without a host transport, arming also starts the own transport.
    void setRecordArmed (bool shouldRecord);
    bool isRecordArmed() const { return recordArmed.load(); }
    bool isRecording() const { return recording.load(); }
    double getRecordedSeconds() const;   // the take being recorded, otherwise the current take's length
    double getBarOffsetBeats() const { return captureSkipBeats.load(); }
    bool wasLastTakeTooShort() const { const juce::ScopedLock lock (stateLock); return lastTakeTooShort; }

    //==========================================================================
    // Audio files as the take. Loading is asynchronous: the file is decoded (first 120 s, at its
    // own sample rate), its tempo estimated when there is no host transport, and analysed on the
    // worker; the new take, key and ideas appear when it is done (getRevision() changes). A load
    // that fails leaves the current session alone and sets getLoadError().
    void loadAudioFile (const juce::File& file);
    bool isLoadingFile() const { return pendingLoads.load() > 0; }
    juce::String getLoadError() const { const juce::ScopedLock lock (stateLock); return loadError; }
    static bool isAudioFile (const juce::File& file);   // by extension: wav, aif(f), flac, ogg, mp3 (if decodable here)
    static juce::String getAudioFileWildcard();         // for a file chooser, e.g. "*.wav;*.aif;..."
    // "Song.wav" for a loaded file, "Recorded take" for a recording, empty when there is no take.
    juce::String getTakeName() const;
    bool isTakeFromFile() const;
    bool hasTake() const;
    bool isAnalysing() const { return pendingAnalyses.load() > 0; } // a tempo change is being applied

    //==========================================================================
    // Transport. With a host transport (a DAW) its tempo and position drive everything and the
    // manual tempo and play/stop below have no effect. Without one (the standalone app) the
    // processor runs its own transport at the manual tempo.
    bool hasHostTransport() const { return hostTransport.load(); }
    double getTempo() const;                                   // the tempo in use (host or manual)
    double getManualTempo() const { return manualBpm.load(); }
    // 20-300 BPM. Without a host transport this re-analyses the take at the new tempo
    // (asynchronously; the ideas are rebuilt, and a progression the user has edited is kept).
    // The tempo the take is already analysed at changes nothing. With a host transport the value
    // is only stored.
    void setManualTempo (double bpm);
    double getEstimatedTempo() const { const juce::ScopedLock lock (stateLock); return estimatedBpm; } // for the loaded file, 0 if none
    bool isTransportPlaying() const { return transportPlaying.load(); }
    // Play starts from the loop start; stop silences the preview. Own transport only.
    void setTransportPlaying (bool shouldPlay);
    static constexpr double minManualTempo = 20.0, maxManualTempo = 300.0, defaultTempo = 120.0;

    //==========================================================================
    // Take playback: the take itself plays in a loop, locked to the chord preview. Each new take
    // turns it on when it came from a file or there was no host transport, and off for takes
    // recorded from a DAW track. The user's choice then stands until the next take.
    bool isTakePlaybackOn() const { return playTake.load(); }
    void setTakePlayback (bool shouldPlay);
    std::atomic<float> takeGain { 1.0f };

    //==========================================================================
    // Session
    bool hasProgression() const;
    bool hasCapture() const;            // an analysed take is present (ideas and alternatives available)
    backline::Key getKey() const;
    void setKey (backline::Key newKey);
    double getBpm() const { const juce::ScopedLock lock (stateLock); return bpm; } // tempo the take was analysed at
    double getBeatsPerBar() const { const juce::ScopedLock lock (stateLock); return beatsPerBar; }
    double getTakeBeats() const { const juce::ScopedLock lock (stateLock); return totalBeats; }
    double getChordRate() const { const juce::ScopedLock lock (stateLock); return chordRate; }   // bars per chord of the ideas
    void setChordRate (double barsPerChord);                                                     // 0.5, 1 or 2; rebuilds the ideas
    double getBeatsPerChord() const { const juce::ScopedLock lock (stateLock); return beatsPerBar * chordRate; }
    int getNumIdeas() const;
    int getSelectedIdea() const { const juce::ScopedLock lock (stateLock); return selectedIdea; }
    void selectIdea (int index);
    backline::ChordRhythm getChordRhythm() const { const juce::ScopedLock lock (stateLock); return chordRhythm; }
    backline::BassRhythm getBassRhythm() const { const juce::ScopedLock lock (stateLock); return bassRhythm; }
    void setChordRhythm (backline::ChordRhythm);
    void setBassRhythm (backline::BassRhythm);

    //==========================================================================
    // The progression, as heard: chord events with start and length in beats from the loop start,
    // contiguous, covering the loop. Draw each card with a width proportional to its length.
    backline::Progression getProgression() const;
    std::vector<backline::Chord> getChords() const;   // just the chords, in order
    int getChordIndexAt (double beat) const;           // e.g. for getPlayheadBeats(); -1 outside the loop
    double getGridBeats() const;                       // edit grid: boundaries snap to multiples of this (1 beat)

    // Edits. Each returns true if the progression changed (false for an impossible edit: index
    // out of range, chord too short, the last chord, or the 64-chord limit). The loop length
    // never changes. Chord indices are positions in getProgression().
    bool setChord (int index, backline::Chord chord);
    bool splitChord (int index);                       // in two at the middle, on the grid
    bool moveChordBoundary (int boundary, double beat); // the boundary between chords `boundary` and `boundary + 1`, to `beat` (snapped)
    bool deleteChord (int index);                      // time goes to the previous chord (the next for the first)
    bool insertPassingChord (int index);               // at the end of chord `index`, leading into the next
    // For a chord's menu: other chords that fit that stretch of the take, best first (the key's
    // chords when there is no take), coloured with sevenths when that is on...
    std::vector<backline::Chord> getAlternatives (int index) const;
    // ...and the same root in other qualities that fit the key, then inversions.
    std::vector<backline::Chord> getVariations (int index) const;

    // Sevenths: turning it on gives every major/minor triad its diatonic 7th (V becomes V7...);
    // turning it off takes back the 7ths it added. The progression holds what is heard, so a chord
    // picked from a menu or inserted as a passing chord is played exactly as picked, and stays so
    // when the setting is turned off. New ideas follow the setting.
    bool getSevenths() const { const juce::ScopedLock lock (stateLock); return sevenths; }
    void setSevenths (bool on);

    //==========================================================================
    // Instruments for the chord and bass parts: instruments::builtInName ("Synth") or the name of
    // a folder under getSoundsFolder() holding samples (see Instruments.h).
    enum class Part { Chords, Bass, Melody };
    // Rescans the sounds folder (call it when an instrument menu opens): the synth first, then the
    // folders found. A selected instrument whose folder has gone falls back to the synth, and one
    // that has (re)appeared is loaded again.
    juce::StringArray getInstrumentNames();
    juce::String getInstrument (Part part) const;        // the one selected (saved with the session)
    juce::String getLoadedInstrument (Part part) const;  // the one sounding: the synth while loading or on fallback
    // Loads asynchronously; Melody is ignored. Asking for the instrument already playing (or on its
    // way) changes nothing; a real change crossfades from the old sound to the new one.
    void setInstrument (Part part, const juce::String& name);
    bool isInstrumentLoading() const { return pendingInstrumentLoads.load() > 0; }
    bool isInstrumentLoading (Part part) const;          // the part's selected instrument is still being loaded
    juce::File getSoundsFolder() const { const juce::ScopedLock lock (stateLock); return soundsFolder; }
    void setSoundsFolder (const juce::File& folder);     // defaults to instruments::defaultSoundsFolder()

    //==========================================================================
    // Export, made for drag and drop. An empty File means there was nothing to export.
    // MIDI goes to a temporary folder unique to this plugin instance and is overwritten by the next
    // export of the same part (DAWs import MIDI as data).
    juce::File writeMidiFile (Part part);
    // Chords or Bass as a 24-bit stereo WAV: one loop at the current tempo plus the release tail,
    // played by the part's instrument, at the device sample rate. DAWs keep dropped audio where it
    // is, so every export is a new file in getExportFolder(), named after the part and the time,
    // and never overwritten; exporting the same unchanged audio again hands out the same file.
    // Nothing (an empty File) while the part's instrument is still loading. Renders on the
    // calling thread (never the audio thread); typically well under a second.
    juce::File writeWavFile (Part part);
    juce::File getExportFolder() const { const juce::ScopedLock lock (stateLock); return exportFolder; }
    void setExportFolder (const juce::File& folder);     // defaults to <Music>/Backline/Exports
    juce::AudioBuffer<float> renderPartAudio (Part part, double sampleRate) const; // what writeWavFile writes
    std::vector<backline::Note> getPartNotes (Part part) const;

    //==========================================================================
    // For the display
    std::vector<float> getWaveform (int numPoints) const;   // peak per point, 0..1; one loop of the take once analysed
    double getLoopBeats() const;
    double getPlayheadBeats() const { return playheadBeats.load(); } // -1 when not playing

    std::atomic<bool> previewEnabled { true };
    std::atomic<float> previewGain { 0.7f };
    int getRevision() const { return revision.load(); }

    static void scheduleNotes (const std::vector<backline::Note>&, juce::MidiBuffer&, double blockStart,
                               double blockEnd, double loopBeats, int numSamples);
    static constexpr int maxChords = 64;

    // The take: audio at its own sample rate, one channel for recordings, up to two for files.
    struct TakeAudio
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        juce::String name;
        bool fromFile = false;
    };

private:
    struct Playback
    {
        std::vector<backline::Note> chords, bass;
        double lengthBeats = 0.0, originPpq = 0.0;
        std::shared_ptr<const TakeAudio> take;      // null when there is no take
        double takeStartSample = 0.0;               // take sample at loop beat 0
        double takeSamplesPerBeat = 0.0;
    };

    // Result of analysing a take (pure data, made off the session lock).
    struct Analysis
    {
        std::shared_ptr<const TakeAudio> take;
        double bpm = 120.0, beatsPerBar = 4.0, originPpq = 0.0, totalBeats = 0.0, estimatedBpm = 0.0;
        int offset = 0;
        backline::ChromaFrames chroma;
        std::vector<backline::Note> melody;
        bool ok() const { return ! chroma.frames.empty(); }
    };
    static Analysis analyseTake (std::shared_ptr<const TakeAudio> take, double bpm, double beatsPerBar,
                                 int offsetSamples, double originPpq, const backline::ChromaFrames* knownChroma);
    static std::shared_ptr<TakeAudio> decodeFile (const juce::File& file, juce::String& error);

    // Reads the take for playback: a position in take samples, fading in or out.
    struct TakeReader
    {
        const TakeAudio* take = nullptr;
        double position = 0.0, increment = 1.0;
        int fade = 0, fadeLength = 1; // fading in: samples done so far; fading out: samples left
        bool active = false;
    };

    void timerCallback() override;
    void analyseCapture();
    void installAnalysis (Analysis&& analysis, bool newTake, bool fromHostRecording);
    void rebuildIdeas (bool keepProgression = false);
    void updatePlayback();
    void haltPreviewAndTake();
    void requestReanalysis();
    bool editProgression (const std::function<backline::Progression (const backline::Progression&)>& edit);
    backline::Chroma chromaForSpan (double startBeat, double lengthBeats) const;
    void installInstrument (int partIndex, instruments::InstrumentPtr instrument);
    void startInstrumentLoad (int partIndex, const juce::String& name);
    void post (std::function<void()> resultForMessageThread);
    void renderTake (juce::AudioBuffer<float>& buffer, int numChannels, int numSamples, bool takeOn,
                     double startBeat, double blockBeats);
    static int partIndex (Part part) { return part == Part::Bass ? 1 : 0; }

    // Capture (written on the audio thread while recording, read on the message thread afterwards)
    std::vector<float> capture;
    std::atomic<int> capturedSamples { 0 };
    std::atomic<bool> recordArmed { false }, recording { false }, analysisPending { false };
    std::atomic<double> captureStartPpq { 0.0 }, captureBpm { 120.0 }, captureSkipBeats { 0.0 },
                        captureBeatsPerBar { 4.0 }, captureSampleRate { 44100.0 }, currentSampleRate { 44100.0 };
    std::atomic<bool> captureHadHostTransport { false };
    double internalPpq = 0.0, expectedRecordPpq = 0.0; // audio thread

    // Transport shared with the message thread
    std::atomic<bool> hostTransport { false }, transportPlaying { true }, playTake { false }, rewindRequested { false };
    std::atomic<double> manualBpm { defaultTempo }, lastHostBpm { defaultTempo }, lastHostBeatsPerBar { 4.0 }, rewindTarget { 0.0 };

    // Session state, guarded by stateLock
    juce::CriticalSection stateLock;
    std::shared_ptr<const TakeAudio> take;
    backline::ChromaFrames chroma;
    std::vector<backline::Chroma> segments;
    backline::Key key;
    double bpm = 120.0, originPpq = 0.0, totalBeats = 0.0, beatsPerBar = 4.0, chordRate = 1.0, estimatedBpm = 0.0;
    int analysisOffset = 0; // take samples skipped so the analysis starts on a bar line
    std::vector<std::vector<backline::Chord>> ideas;
    backline::Progression progression;
    std::vector<backline::Note> melodyNotes;
    int selectedIdea = 0;
    bool lastTakeTooShort = false, sevenths = false;
    bool progressionEdited = false; // changed by the user since an idea was last put in place
    backline::ChordRhythm chordRhythm = backline::ChordRhythm::Sustain;
    backline::BassRhythm bassRhythm = backline::BassRhythm::Eighths;
    juce::String loadError;
    juce::File soundsFolder, exportFolder;
    juce::String requestedInstrument[2];
    juce::String loadingInstrument[2];  // the instrument being loaded for each part, empty when none
    instruments::InstrumentPtr loadedInstrument[2];
    struct WavExport
    {
        juce::File file;
        juce::uint64 hash = 0;
        juce::int64 size = 0;
        juce::Time modified;
    };
    WavExport lastWav[2];               // the last WAV handed out for each part
    int loadTicket = 0, analysisTicket = 0, instrumentTicket[2] = { 0, 0 };
    std::atomic<int> revision { 0 };
    std::atomic<double> playheadBeats { -1.0 };
    std::atomic<int> pendingLoads { 0 }, pendingAnalyses { 0 }, pendingInstrumentLoads { 0 };

    // Work finished on the worker, waiting to be installed on the message thread.
    juce::CriticalSection resultLock;
    std::vector<std::function<void()>> finishedWork;

    // Preview playback: the message thread publishes, the audio thread picks up and hands the old one back.
    juce::SpinLock playbackLock;
    std::shared_ptr<const Playback> pendingPlayback, releasedPlayback;     // guarded by playbackLock
    std::unique_ptr<juce::Synthesiser> pendingSynth[2], releasedSynth[2];  // guarded by playbackLock
    std::shared_ptr<const Playback> activePlayback;                        // audio thread
    std::unique_ptr<juce::Synthesiser> activeSynth[2];                     // audio thread (chords, bass)
    std::unique_ptr<juce::Synthesiser> fadingSynth[2];                     // audio thread: the instrument just replaced, fading out
    int fadeSamplesLeft[2] = { 0, 0 }, fadeSamples[2] = { 1, 1 };          // audio thread
    bool wasPlaying = false, takeWasPlaying = false, takeSwapWaiting = false;
    double nextBeat = -1.0, takeNextBeat = -1.0; // where the last block ended, -1 after any interruption
    TakeReader takeMain, takeTail;               // the loop being played, and a fading-out continuation
    juce::MidiBuffer chordMidi, bassMidi, noMidi;
    std::atomic<bool> midiOutEnabled { true };
    bool midiOutWasOn = false; // audio thread only
    juce::AudioBuffer<float> synthBuffer, fadeBuffer;

    juce::ThreadPool worker { 1 }; // last, so it is stopped before anything its jobs touch is destroyed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BacklineProcessor)
};
