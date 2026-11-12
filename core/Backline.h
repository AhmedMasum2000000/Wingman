// Backline core: audio analysis and harmony generation. No external dependencies.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace backline
{
using Chroma = std::array<float, 12>;

struct Key
{
    int tonic = 0;       // pitch class, 0 = C
    bool minor = false;
    float confidence = 0.0f;
};

enum class Quality { Major, Minor, Dominant7, Major7, Minor7, Sus2, Sus4, Diminished };
constexpr int numQualities = 8;

struct Chord
{
    int root = 0;                     // pitch class, 0 = C
    Quality quality = Quality::Major;
    int bass = -1;                    // pitch class of the bass note (inversion / slash chord), -1 = the root

    // The bass note as a pitch class, whether or not it was given.
    int bassPitchClass() const { return (((bass < 0 ? root : bass) % 12) + 12) % 12; }
    bool operator== (const Chord& o) const
    {
        return (((root - o.root) % 12) == 0) && quality == o.quality && bassPitchClass() == o.bassPitchClass();
    }
    bool operator!= (const Chord& o) const { return ! (*this == o); }
};

// One chord of a progression. Times are in beats from the start of the loop.
struct ChordEvent
{
    Chord chord;
    double start = 0.0;
    double length = 4.0;
    // Equal when the chord is the same and the times agree to a billionth of a beat.
    bool operator== (const ChordEvent& o) const
    {
        const double ds = start - o.start, dl = length - o.length;
        return chord == o.chord && ds < 1.0e-9 && ds > -1.0e-9 && dl < 1.0e-9 && dl > -1.0e-9;
    }
    bool operator!= (const ChordEvent& o) const { return ! (*this == o); }
};

// Chords in time order. A valid progression starts at beat 0, has no gaps or overlaps and
// every chord is longer than nothing; its end is the length of the loop.
using Progression = std::vector<ChordEvent>;

// Times and lengths are in beats (quarter notes) from the start of the capture.
struct Note
{
    int pitch = 60;
    double start = 0.0;
    double length = 1.0;
    int velocity = 100;
};

enum class ChordRhythm { Sustain, Quarters, Offbeat, Dotted };
enum class BassRhythm { Sustain, Eighths, Octaves, RootFifth, Offbeat };

struct ChromaFrames
{
    std::vector<Chroma> frames;
    double hopSeconds = 0.0;
    double windowSeconds = 0.0;
};

// Note names. The one-argument forms use a fixed neutral spelling (C C# D Eb E F F# G Ab A Bb B);
// the forms that take a key spell the note the way that key is written (G# in E major, Ab in Eb major).
std::string pitchClassName (int pitchClass);
std::string pitchClassName (int pitchClass, Key key);
// C, Cm, C7, Cmaj7, Cm7, Csus2, Csus4, Cdim, and C/E when the bass is not the root.
std::string chordName (Chord chord);
std::string chordName (Chord chord, Key key);
std::string qualitySuffix (Quality quality);       // "", "m", "7", "maj7", "m7", "sus2", "sus4", "dim"
std::string keyName (Key key); // conventional spelling, e.g. "F# minor", "Bb major"

// What a chord is made of.
std::vector<int> chordIntervals (Quality quality); // semitones above the root, root first: { 0, 4, 7, 10 } for a dominant 7th
std::vector<int> chordPitchClasses (Chord chord);  // the chord tones, root first (the bass note is not added)
bool hasSeventh (Quality quality);
bool hasMinorThird (Quality quality);              // Minor, Minor7 and Diminished (written in lower case as a numeral)
bool isInScale (int pitchClass, Key key);          // major scale, or natural minor

// Analysis
ChromaFrames computeChroma (const float* mono, size_t numSamples, double sampleRate);
Key detectKey (const ChromaFrames& chroma);
std::vector<Chroma> segmentChroma (const ChromaFrames& chroma, double bpm,
                                   double beatsPerSegment, int numSegments);
std::vector<Note> audioToNotes (const float* mono, size_t numSamples, double sampleRate, double bpm);
// Tempo of a recording in BPM, always within minTempo..maxTempo: a pulse outside that range comes
// back as its half or double (60 as 120, 200 as 100). Returns 0 when no pulse can be found
// (silence, noise, one held note, less than two seconds of audio). Only the first minute is
// looked at. A result within 0.15% of a whole number is rounded to it.
// Where the beat is ambiguous the reading nearer 120 wins: a loop with a backbeat above about
// 155 BPM is read at half speed, one with busy offbeats below about 75 BPM at double speed.
constexpr double minTempo = 70.0, maxTempo = 180.0;
double estimateTempo (const float* mono, size_t numSamples, double sampleRate);

// Harmony
std::vector<Chord> chordsInKey (Key key);
std::vector<Chord> rankChords (const Chroma& segment, Key key); // best fit first
// Up to `count` progressions, best first, no two the same. Fewer are returned only when the
// segments and the key's chords do not allow `count` different progressions.
std::vector<std::vector<Chord>> suggestProgressions (const std::vector<Chroma>& segments,
                                                     Key key, int count);


// Progressions. Everything below is a pure function: it returns a new value and leaves its
// argument alone. An edit that cannot be made (index out of range, chord too short, last chord)
// returns the progression unchanged, so a result is always valid if the input was.
Progression toProgression (const std::vector<Chord>& chords, double beatsPerChord); // fixed grid to events
std::vector<Chord> chordsOf (const Progression& progression);
double progressionLength (const Progression& progression);                         // loop length in beats
bool isValidProgression (const Progression& progression);                          // see Progression; empty is not valid
int chordIndexAt (const Progression& progression, double beat);                    // -1 outside the loop

// Replace one chord, keeping its place and length.
Progression setChord (const Progression& progression, size_t index, Chord chord);
// Cut a chord in two at its middle, snapped to `gridBeats` (0 = exact middle). Both parts keep the
// chord and are at least one grid step long; a chord shorter than two steps is left whole.
Progression splitChord (const Progression& progression, size_t index, double gridBeats);
// Move the boundary between chord `boundary` and chord `boundary + 1` to `beat` (from the loop
// start), snapped to `gridBeats`. Both chords keep at least one grid step.
Progression moveBoundary (const Progression& progression, size_t boundary, double beat, double gridBeats);
// Remove a chord. Its time goes to the chord before it, or to the next one if it was the first.
// The last remaining chord cannot be deleted.
Progression deleteChord (const Progression& progression, size_t index);
// Add a passing chord at the end of chord `index`, leading into the chord after it (the first
// chord when `index` is the last: the progression loops). It takes the chord's last beat, or its
// second half if the chord is shorter than two beats. Chords shorter than a quarter of a beat
// are left alone.
Progression insertPassingChord (const Progression& progression, size_t index, Key key);
// The chord insertPassingChord uses: a diatonic chord or inversion that fills in a third, a
// passing diminished chord between two roots a tone apart, otherwise the dominant 7th of `to`
// (its leading-note diminished chord if `from` already is that dominant). Never equal to `from` or `to`.
Chord passingChord (Chord from, Chord to, Key key);

// Other forms of one chord for a "change chord type" menu: the same root in every quality whose
// notes belong to the key (in a minor key the raised 7th counts as well; for a root outside the
// key, every quality), in root position, followed by the chord with its third and its fifth in
// the bass. The chord itself is not in the list.
std::vector<Chord> chordVariations (Chord chord, Key key);

// The "sevenths" colouring. A major or minor triad gets the 7th the key gives it (I becomes
// Imaj7, ii becomes ii7, V becomes V7); a minor triad whose diatonic 7th is major, a triad on a
// root outside the key, and chords that are not plain triads stay as they are. The bass is kept.
Chord seventhChord (Chord chord, Key key);
Progression withSevenths (const Progression& progression, Key key);
// The reverse: Dominant7 and Major7 become Major, Minor7 becomes Minor.
Chord triadChord (Chord chord);
Progression withoutSevenths (const Progression& progression);

// Parts. Each chord plays the rhythm from its own start and always sounds at least once.
// Chord voicings hold every chord tone (four notes for 7th chords), stay close to the previous
// voicing, and put the bass note of a slash chord at the bottom. The bass part plays the bass note.
std::vector<Note> renderChords (const Progression& progression, ChordRhythm rhythm);
std::vector<Note> renderBass (const Progression& progression, BassRhythm rhythm);
// Fixed grid shorthand for renderChords (toProgression (chords, beatsPerChord), rhythm).
std::vector<Note> renderChords (const std::vector<Chord>& progression, double beatsPerChord, ChordRhythm rhythm);
std::vector<Note> renderBass (const std::vector<Chord>& progression, double beatsPerChord, BassRhythm rhythm);

// Standard MIDI file (type 0)
std::vector<uint8_t> writeMidiFile (const std::vector<Note>& notes, double bpm);
}
