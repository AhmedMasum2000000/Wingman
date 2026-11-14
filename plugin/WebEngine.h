#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <vector>

// Plays the parts the M1D1 interface (the web page in the editor) sends: MIDI out on each part's channel,
// and a simple preview sound for the parts whose Play is on. It follows the host's transport; when the
// host is stopped and a part's Play is on, it runs its own clock from where the interface was.
// It also keeps the last minute of the track's audio the host played, for "Listen to track".
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
    std::atomic<int> reportNum { 4 }, reportDen { 4 };   // the host's time signature

    // The track's recent audio (about 22 kHz), starting on a bar line, with where it is in the song, so the
    // interface can read the chords on the host's own grid:
    //   samples  the mid (L+R)/2;  side  the side (L-R)/2, so parts spread wide in stereo are not lost
    //   segments where each stretch starts: { second in the capture, song beat (host ppq) }. A host loop starts a
    //            new segment, so every pass is placed at its real song beats.
    //   level    the RMS of the mid, to tell silence apart.  Empty if less than 4 seconds were heard.
    struct Segment { double sec = 0, beat = 0; };
    struct Capture { std::vector<float> samples, side; std::vector<Segment> segments; double sampleRate = 0, startBeat = 0, bpm = 120, beatsPerBar = 4, level = 0; };
    Capture getCapture();
    // how much has been heard so far, without copying it: song beat of the first sample and the number of beats
    std::atomic<double> capturedFromBeat { 0.0 }, capturedBeats { 0.0 };
    static constexpr double captureSeconds = 64.0;   // a whole 8-bar stretch of the song even at 60 BPM

private:
    struct Voice { int part = -1, pitch = 0; double phase = 0, inc = 0; float level = 0, gain = 0, z1 = 0, z2 = 0; bool releasing = false; };
    struct Event { int offset; int part; int pitch; int velocity; bool on; };

    void collectEvents (const Song& song, double from, double to, int numSamples);
    void startVoice (int part, int pitch, int velocity);
    void stopVoice (int part, int pitch);
    void render (juce::AudioBuffer<float>& buffer, int start, int end);
    void releaseHeld (juce::MidiBuffer& midi, int offset);
    void releasePart (int part, juce::MidiBuffer& midi, int offset);
    // After a start, a seek, a loop or new notes: what should be sounding at `from` (started before it and still
    // going) is made to sound, notes that should not are released, and notes already right are left alone.
    // restrike: after a jump, a note struck before the last two blocks is struck again (the music came round to it);
    // after an edit, a note still wanted keeps sounding.
    void reconcile (const Song& song, std::array<bool, 3> parts, double from, juce::MidiBuffer& midi, bool restrike);
    bool voiceOn (int part, int pitch) const;

    double sampleRate = 44100.0;
    std::atomic<const Song*> current { nullptr };
    std::deque<std::shared_ptr<Song>> keepAlive;   // message thread: the current song and the one the audio thread uses
    std::atomic<const Song*> inUse { nullptr };      // published by the audio thread before it reads a song

    std::atomic<bool> previewRequest { false };
    std::atomic<double> previewStartBeat { 0.0 }, previewTempo { 120.0 };
    std::atomic<int> previewTicket { 0 };
    int previewSeen = 0;
    bool previewRunning = false, wasPlaying = false, wasHostClock = false;
    double previewBeat = 0.0, expectedBeat = -1.0;

    std::array<Voice, 32> voices;
    std::array<Event, 1024> events; int numEvents = 0;
    std::array<std::array<bool, 128>, 3> held {};   // notes on (sent as MIDI) per part
    std::array<std::array<juce::int64, 128>, 3> heldAt {};   // when each was struck, in samples processed
    juce::int64 samplesDone = 0; int lastBlock = 0;
    std::array<int, 3> heldChannel { 2, 1, 3 };
    const Song* lastSong = nullptr;   // only compared, never read: the previous song's parts are kept in prevParts
    std::array<Part, 3> prevParts;    // a copy, in storage reserved up front (no allocation on the audio thread)
    bool havePrev = false;

    // capture: mid and side, decimated to about 22 kHz, written while the host plays
    juce::SpinLock captureLock;
    std::vector<float> ring, ringSide; size_t ringWrite = 0, ringCount = 0, ringTotal = 0;
    struct Mark { size_t total; double ppq; };      // a segment: from ring sample `total` on, the song is at `ppq`
    std::vector<Mark> marks;                         // oldest first; at most maxMarks
    static constexpr size_t maxMarks = 512;
    bool captureWasPlaying = false; double captureExpectedPpq = 0;
    int decim = 2, decimCount = 0, captureGap = 0;   // captureGap: input samples a busy block could not write
    float decimSum = 0, decimSide = 0;
    double captureBpm = 120, captureBeatsPerBar = 4;
    void captureReset (double ppq, double bpm, double beatsPerBar);
    // what a block could not apply while the interface held the capture: a new recording, and loop marks (at their place
    // in the gap, counted in input samples)
    bool pendRestart = false; double pendPpq = 0, pendBpm = 120, pendBpb = 4;
    struct PendMark { int at; double ppq; }; std::array<PendMark, 32> pendMarks {}; int numPend = 0;
    void capturePush (float mid, float side);
};
} // namespace m1d1
