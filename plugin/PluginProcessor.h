#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Backline.h"

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
    const juce::String getName() const override { return "Backline"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Recording
    void setRecordArmed (bool shouldRecord);
    bool isRecordArmed() const { return recordArmed.load(); }
    bool isRecording() const { return recording.load(); }
    double getRecordedSeconds() const;
    double getBarOffsetBeats() const { return captureSkipBeats.load(); }
    bool wasLastTakeTooShort() const { const juce::ScopedLock lock (stateLock); return lastTakeTooShort; }

    // Session
    bool hasProgression() const;
    bool hasCapture() const;
    backline::Key getKey() const;
    void setKey (backline::Key newKey);
    double getBpm() const { const juce::ScopedLock lock (stateLock); return bpm; }
    double getBeatsPerBar() const { const juce::ScopedLock lock (stateLock); return beatsPerBar; }
    double getTakeBeats() const { const juce::ScopedLock lock (stateLock); return totalBeats; }
    double getChordRate() const { const juce::ScopedLock lock (stateLock); return chordRate; }   // bars per chord
    void setChordRate (double barsPerChord);                                                     // 0.5, 1 or 2
    double getBeatsPerChord() const { const juce::ScopedLock lock (stateLock); return beatsPerBar * chordRate; }
    int getNumIdeas() const;
    int getSelectedIdea() const { const juce::ScopedLock lock (stateLock); return selectedIdea; }
    void selectIdea (int index);
    std::vector<backline::Chord> getProgression() const;
    std::vector<backline::Chord> getAlternatives (int segment) const;
    void setChord (int segment, backline::Chord chord);
    backline::ChordRhythm getChordRhythm() const { const juce::ScopedLock lock (stateLock); return chordRhythm; }
    backline::BassRhythm getBassRhythm() const { const juce::ScopedLock lock (stateLock); return bassRhythm; }
    void setChordRhythm (backline::ChordRhythm);
    void setBassRhythm (backline::BassRhythm);

    enum class Part { Chords, Bass, Melody };
    juce::File writeMidiFile (Part part);
    std::vector<backline::Note> getPartNotes (Part part) const;

    // For the display
    std::vector<float> getWaveform (int numPoints) const;   // peak per point, 0..1
    double getLoopBeats() const;
    double getPlayheadBeats() const { return playheadBeats.load(); } // -1 when not playing

    std::atomic<bool> previewEnabled { true };
    std::atomic<float> previewGain { 0.7f };
    int getRevision() const { return revision.load(); }

    static void scheduleNotes (const std::vector<backline::Note>&, juce::MidiBuffer&, double blockStart,
                               double blockEnd, double loopBeats, int numSamples);
    static constexpr int maxChords = 64;

private:
    struct Playback
    {
        std::vector<backline::Note> chords, bass;
        double lengthBeats = 0.0, originPpq = 0.0;
    };

    void timerCallback() override;
    void analyseCapture();
    void rebuildIdeas();
    void updatePlayback();
    void haltPreviewAndTake();

    // Capture (written on the audio thread while recording, read on the message thread afterwards)
    std::vector<float> capture;
    std::atomic<int> capturedSamples { 0 };
    std::atomic<bool> recordArmed { false }, recording { false }, analysisPending { false };
    std::atomic<double> captureStartPpq { 0.0 }, captureBpm { 120.0 }, captureSkipBeats { 0.0 },
                        captureBeatsPerBar { 4.0 }, captureSampleRate { 44100.0 }, currentSampleRate { 44100.0 };
    double internalPpq = 0.0, expectedRecordPpq = 0.0; // audio thread

    // Session state, guarded by stateLock
    juce::CriticalSection stateLock;
    backline::ChromaFrames chroma;
    std::vector<backline::Chroma> segments;
    backline::Key key;
    double bpm = 120.0, originPpq = 0.0, totalBeats = 0.0, beatsPerBar = 4.0, chordRate = 1.0;
    int analysisOffset = 0; // samples skipped so the analysis starts on a bar line
    std::vector<std::vector<backline::Chord>> ideas;
    std::vector<backline::Chord> progression;
    std::vector<backline::Note> melodyNotes;
    int selectedIdea = 0;
    bool lastTakeTooShort = false;
    backline::ChordRhythm chordRhythm = backline::ChordRhythm::Sustain;
    backline::BassRhythm bassRhythm = backline::BassRhythm::Eighths;
    std::atomic<int> revision { 0 };
    std::atomic<double> playheadBeats { -1.0 };

    // Preview playback: the message thread publishes, the audio thread picks up and hands the old one back.
    juce::SpinLock playbackLock;
    std::shared_ptr<const Playback> pendingPlayback, releasedPlayback; // guarded by playbackLock
    std::shared_ptr<const Playback> activePlayback;                   // audio thread
    bool wasPlaying = false;
    double nextBeat = -1.0; // where the last preview block ended, -1 after any interruption
    juce::Synthesiser chordSynth, bassSynth;
    juce::MidiBuffer chordMidi, bassMidi;
    juce::AudioBuffer<float> synthBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BacklineProcessor)
};
