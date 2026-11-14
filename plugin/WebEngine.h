#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <vector>

// Plays the parts the M1D1 interface (the web page in the editor) sends: MIDI out on each part's channel,
// and a simple preview sound for the parts whose Play is on. It follows the host's transport; when the
// host is stopped and a part's Play is on, it runs its own clock from where the interface was.
// It also keeps the last half minute of the track's audio the host played, for "Listen to track".
namespace m1d1
{
struct Note { double start = 0, length = 0; int pitch = 60, velocity = 100; };
struct Part { int channel = 1; bool sound = false, midi = true; std::vector<Note> notes; };   // midi: sent as MIDI out
struct Song
{
    double loopBeats = 16.0, tempo = 120.0;
    std::array<Part, 3> parts;   // bass (ch 2), chords (ch 1), lead (ch 3), in the interface's order
};

// The song as the interface sends it: {"loopBeats":16,"tempo":124,"parts":[{"ch":2,"sound":false,"notes":[[x,len,pitch,vel],...]},...]}
std::shared_ptr<Song> parseSong (const juce::String& json);

class Engine
{
public:
    Engine();
    void prepare (double sampleRate, int maxBlock);

    // message thread
    void setSong (std::shared_ptr<Song> song);
    void setPreview (bool run, double beat, double tempo);
    bool hasSong() const { return current.load() != nullptr; }

    // audio thread: adds the parts' sound to the buffer and their MIDI to midi
    void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, juce::AudioPlayHead* playHead, int numInputChannels);
    void stopAll (juce::MidiBuffer& midi);   // releases every note (bypass, reset)

    // where the music is, for the interface
    std::atomic<double> reportBeat { 0.0 }, reportBpm { 120.0 };
    std::atomic<bool> reportHostPlaying { false }, reportPreview { false };

    // The track's recent audio as mono (about 22 kHz), starting on a bar line, with where that is in the song:
    // startBeat is the song beat (host ppq) of the first sample, so the interface can read the chords on the
    // host's own grid. Empty if less than 4 seconds were heard. level is the RMS, to tell silence apart.
    struct Capture { std::vector<float> samples; double sampleRate = 0, startBeat = 0, bpm = 120, beatsPerBar = 4, level = 0; };
    Capture getCapture();
    // how much has been heard so far, without copying it: song beat of the first sample and the number of beats
    std::atomic<double> capturedFromBeat { 0.0 }, capturedBeats { 0.0 };
    static constexpr double captureSeconds = 32.0;

private:
    struct Voice { int part = -1, pitch = 0; double phase = 0, inc = 0; float level = 0, gain = 0, z1 = 0, z2 = 0; bool releasing = false; };
    struct Event { int offset; int part; int pitch; int velocity; bool on; };

    void collectEvents (const Song& song, double from, double to, int numSamples);
    void startVoice (int part, int pitch, int velocity);
    void stopVoice (int part, int pitch);
    void render (juce::AudioBuffer<float>& buffer, int start, int end);
    void releaseHeld (juce::MidiBuffer& midi, int offset);

    double sampleRate = 44100.0;
    std::atomic<const Song*> current { nullptr };
    std::deque<std::shared_ptr<Song>> keepAlive;   // message thread: songs the audio thread may still read

    std::atomic<bool> previewRequest { false };
    std::atomic<double> previewStartBeat { 0.0 }, previewTempo { 120.0 };
    std::atomic<int> previewTicket { 0 };
    int previewSeen = 0;
    bool previewRunning = false, wasPlaying = false;
    double previewBeat = 0.0, expectedBeat = -1.0;

    std::array<Voice, 32> voices;
    std::array<Event, 1024> events; int numEvents = 0;
    std::array<std::array<bool, 128>, 3> held {};   // notes on (sent as MIDI) per part
    std::array<int, 3> heldChannel { 2, 1, 3 };
    const Song* lastSong = nullptr;

    // capture: mono, decimated to about 22 kHz, written while the host plays
    juce::SpinLock captureLock;
    std::vector<float> ring; size_t ringWrite = 0, ringCount = 0, ringTotal = 0;
    bool captureWasPlaying = false; double captureExpectedPpq = 0;
    int decim = 2, decimCount = 0; float decimSum = 0;
    double captureStartPpq = 0, captureBpm = 120, captureBeatsPerBar = 4;
};
} // namespace m1d1
