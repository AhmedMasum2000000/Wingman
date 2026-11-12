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
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
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

    // Session (message thread only)
    void setRecordArmed (bool shouldRecord);
    bool isRecordArmed() const { return recordArmed.load(); }
    bool isRecording() const { return recording.load(); }
    double getRecordedSeconds() const;

    bool hasProgression() const { return ! progression.empty(); }
    bool hasCapture() const { return ! segments.empty(); }
    backline::Key getKey() const { return key; }
    void setKey (backline::Key newKey);
    double getBpm() const { return bpm; }
    double getBeatsPerChord() const { return beatsPerChord; }
    void setBeatsPerChord (double beats);
    int getNumIdeas() const { return (int) ideas.size(); }
    int getSelectedIdea() const { return selectedIdea; }
    void selectIdea (int index);
    const std::vector<backline::Chord>& getProgression() const { return progression; }
    std::vector<backline::Chord> getAlternatives (int segment) const;
    void setChord (int segment, backline::Chord chord);
    backline::ChordRhythm getChordRhythm() const { return chordRhythm; }
    backline::BassRhythm getBassRhythm() const { return bassRhythm; }
    void setChordRhythm (backline::ChordRhythm r) { chordRhythm = r; updatePlayback(); }
    void setBassRhythm (backline::BassRhythm r) { bassRhythm = r; updatePlayback(); }

    enum class Part { Chords, Bass, Melody };
    juce::File writeMidiFile (Part part);
    std::vector<backline::Note> getPartNotes (Part part) const;

    // For the display
    std::vector<float> getWaveform (int numPoints) const;   // peak per point, 0..1
    double getLoopBeats() const { return (double) progression.size() * beatsPerChord; }
    double getPlayheadBeats() const { return playheadBeats.load(); } // -1 when not playing

    std::atomic<bool> previewEnabled { true };
    std::atomic<float> previewGain { 0.7f };
    int getRevision() const { return revision; }

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
    void scheduleNotes (const std::vector<backline::Note>&, juce::MidiBuffer&, double startBeat,
                        double blockBeats, double loopBeats, int numSamples) const;

    // Capture (written on the audio thread while recording, read on the message thread afterwards)
    std::vector<float> capture;
    std::atomic<int> capturedSamples { 0 };
    std::atomic<bool> recordArmed { false }, recording { false }, analysisPending { false };
    std::atomic<double> captureStartPpq { 0.0 }, captureBpm { 120.0 };
    double currentSampleRate = 44100.0, internalPpq = 0.0;

    // Session state (message thread)
    backline::ChromaFrames chroma;
    std::vector<backline::Chroma> segments;
    backline::Key key;
    double bpm = 120.0, originPpq = 0.0, totalBeats = 0.0, beatsPerChord = 4.0;
    std::vector<std::vector<backline::Chord>> ideas;
    std::vector<backline::Chord> progression;
    std::vector<backline::Note> melodyNotes;
    std::atomic<double> playheadBeats { -1.0 };
    int selectedIdea = 0, revision = 0;
    backline::ChordRhythm chordRhythm = backline::ChordRhythm::Sustain;
    backline::BassRhythm bassRhythm = backline::BassRhythm::Eighths;

    // Preview playback: the message thread publishes, the audio thread picks up.
    juce::SpinLock playbackLock;
    std::shared_ptr<const Playback> pendingPlayback, retiredPlayback; // message thread keeps refs alive
    std::shared_ptr<const Playback> activePlayback;                   // audio thread
    bool wasPlaying = false;
    juce::Synthesiser chordSynth, bassSynth;
    juce::MidiBuffer chordMidi, bassMidi;
    juce::AudioBuffer<float> synthBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BacklineProcessor)
};
