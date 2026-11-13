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

// The first eight are the basic chords offered by chordVariations; the 9th chords after them are
// colours that the variation engine adds (Cadd9, Cmadd9, Cmaj9, Cm9, C9).
enum class Quality { Major, Minor, Dominant7, Major7, Minor7, Sus2, Sus4, Diminished,
                     Add9, MinorAdd9, Major9, Minor9, Dominant9 };
constexpr int numQualities = 13;
constexpr int numBasicQualities = 8;

struct Chord
{
    int root = 0;                     // pitch class, 0 = C
    Quality quality = Quality::Major;
    int bass = -1;                    // pitch class of the bass note (inversion / slash chord), -1 = the root
    // Spelling only: a root outside the key is written with a sharp (F#dim, not Gbdim, for a
    // chord that leads up a semitone). Not part of ==.
    bool sharpRoot = false;

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
    // The chord's 7th was added by withSevenths, so withoutSevenths takes it away again. Every
    // other edit of the chord clears it. Not part of ==.
    bool coloured = false;
    // How sure the analysis is of this chord, 0..1 (1 for chords that were not detected). Not part of ==.
    float confidence = 1.0f;
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
    // Glide into this note from the one before it: the note before ends where this one starts, and
    // a MIDI output can overlap the two slightly (legato) so a mono synth with glide slides.
    bool slide = false;
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
// C, Cm, C7, Cmaj7, Cm7, Csus2, Csus4, Cdim, Cadd9, Cmadd9, Cmaj9, Cm9, C9, and C/E when the bass
// is not the root.
std::string chordName (Chord chord);
std::string chordName (Chord chord, Key key);
std::string qualitySuffix (Quality quality);       // "", "m", "7", "maj7", "m7", "sus2", "sus4", "dim", "add9", "madd9", "maj9", "m9", "9"
std::string keyName (Key key); // conventional spelling, e.g. "F# minor", "Bb major"

// What a chord is made of.
std::vector<int> chordIntervals (Quality quality); // semitones above the root, root first: { 0, 4, 7, 10 } for a dominant 7th, { 0, 4, 7, 14 } for add9
std::vector<int> chordPitchClasses (Chord chord);  // the chord tones, root first (the bass note is not added)
bool hasSeventh (Quality quality);                 // the 7th chords and the 9th chords that hold a 7th
bool hasMinorThird (Quality quality);              // Minor, Minor7, Diminished, MinorAdd9, Minor9 (written in lower case as a numeral)
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
// "Regenerate": the same, but each segment is heard a little differently, so other plausible
// ideas come up. Seed 0 gives exactly the ideas above; each other seed its own set.
std::vector<std::vector<Chord>> suggestProgressions (const std::vector<Chroma>& segments,
                                                     Key key, int count, std::uint32_t seed);


// Progressions. Everything below is a pure function: it returns a new value and leaves its
// argument alone. An edit that cannot be made (index out of range, chord too short, last chord)
// returns the progression unchanged, so a result is always valid if the input was. Edits that
// only move times (split, move, delete, insert) keep each chord's `coloured` flag.
Progression toProgression (const std::vector<Chord>& chords, double beatsPerChord); // fixed grid to events
std::vector<Chord> chordsOf (const Progression& progression);
double progressionLength (const Progression& progression);                         // loop length in beats
bool isValidProgression (const Progression& progression);                          // see Progression; empty is not valid
int chordIndexAt (const Progression& progression, double beat);                    // -1 outside the loop

// Replace one chord, keeping its place and length. The new chord is the user's (not coloured).
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
// The diminished chords lead up a semitone, so they are spelled with a sharp (sharpRoot).
Chord passingChord (Chord from, Chord to, Key key);

// Other forms of one chord for a "change chord type" menu: the same root in every basic quality whose
// notes belong to the key (in a minor key the raised 7th counts as well; for a root outside the
// key, every quality), in root position, followed by the chord with its third and its fifth in
// the bass. The chord itself is not in the list. They are spelled like the chord (sharpRoot).
std::vector<Chord> chordVariations (Chord chord, Key key);

// The "sevenths" colouring. A major or minor triad gets the 7th the key gives it (I becomes
// Imaj7, ii becomes ii7, V becomes V7); a minor triad whose diatonic 7th is major, a triad on a
// root outside the key, and chords that are not plain triads stay as they are. The bass is kept.
Chord seventhChord (Chord chord, Key key);
// Colours every chord and marks the ones it changed (`coloured`).
Progression withSevenths (const Progression& progression, Key key);
// The reverse: Dominant7, Major7 and the major 9th chords become Major, Minor7 and the minor 9th chords Minor.
Chord triadChord (Chord chord);
// Takes back only the 7ths withSevenths added: a 7th chord that was picked or inserted (a passing
// dominant, a choice from a menu) stays as it is.
Progression withoutSevenths (const Progression& progression);

// Parts. Every chord always sounds at least once. Sustain, quarters and offbeats play from each
// chord's own start; the dotted figure (0, 1.5 and 3 in 4/4; 0 and 1.5 in 3/4) follows the bar
// lines of the loop, `beatsPerBar` apart, and a chord that starts between its hits is struck on
// its start as well. Chord voicings hold every chord tone (four notes for 7th chords), stay close
// to the previous voicing, and put the bass note of a slash chord at the bottom. The bass part
// plays the bass note.
std::vector<Note> renderChords (const Progression& progression, ChordRhythm rhythm, double beatsPerBar = 4.0);
std::vector<Note> renderBass (const Progression& progression, BassRhythm rhythm);
// Fixed grid shorthand for renderChords (toProgression (chords, beatsPerChord), rhythm).
std::vector<Note> renderChords (const std::vector<Chord>& progression, double beatsPerChord, ChordRhythm rhythm);
std::vector<Note> renderBass (const std::vector<Chord>& progression, double beatsPerChord, BassRhythm rhythm);

// Standard MIDI file (type 0)
std::vector<uint8_t> writeMidiFile (const std::vector<Note>& notes, double bpm);

// ---- Detailed chord timing (Timing.cpp) ----------------------------------------------------
// "Auto" chord timing: chords follow the harmonic changes in the audio, down to one analysis
// step (one beat, or half a beat below 90 BPM). The audio is assumed to start on a bar line.

// Chroma measured once per analysis step, in time with the beat. `chroma` covers the whole
// spectrum (C2 to about C7), `bass` only the bass register (below about 200 Hz), so an inversion
// shows as a bass note that is not the chord's root. Each chroma sums to 1 (all zero where the
// audio is silent); `energy` is the RMS level of each step.
struct BeatChroma
{
    double stepBeats = 1.0;
    double lengthBeats = 0.0;           // the loop the steps cover
    std::vector<Chroma> chroma, bass;
    std::vector<float> energy;
    double tuningCents = 0.0;           // the recording's reference pitch against A = 440 Hz (-50..50)
};
// One beat, or half a beat below 90 BPM where a beat is long enough for two chords.
double analysisStepBeats (double bpm);
// Measures `loopBeats` beats of audio (or as many as there are when loopBeats <= 0). Steps after
// the end of the audio are silent.
BeatChroma analyseBeats (const float* mono, size_t numSamples, double sampleRate, double bpm, double loopBeats);

// The chord heard in each part of the loop, as a progression of variable-length events lined up
// with the analysis steps. `detail` runs from 0 (Simple: changes mostly on bar lines, short
// passing chords smoothed over) to 1 (Detailed: every change, as short as one step). Repeats are
// merged, slash chords come from the bass register, and every event carries a confidence.
// The cost of a change falls as the detail rises, so more detail gives as many chords or more.
// Silence takes the chord around it (the tonic if all is silent).
constexpr double defaultDetail = 0.6;
Progression detectChordTimeline (const BeatChroma& beats, Key key, double detail, double beatsPerBar = 4.0);
// Ideas for Auto timing, all on the detected timing: the first follows the audio as closely as
// it can (detectChordTimeline); the others put plausible diatonic alternatives in its place.
// No two have the same chords. A different `seed` gives different alternatives (the first stays).
std::vector<Progression> suggestTimedIdeas (const BeatChroma& beats, Key key, double detail, int count,
                                            std::uint32_t seed = 0, double beatsPerBar = 4.0);

// ---- Variations (Variation.cpp) ------------------------------------------------------------

// A chord that can be played: root 0..11 and a bass that is the root or one of its chord tones.
bool isValidChord (Chord chord);
// A variation of a progression. Timing and loop length never change. `amount` 0..1: 0 returns the
// progression unchanged; below 0.4 only the colour of chords changes (inversions, 7ths, 9ths,
// sus) and every root stays; above it chords may also be substituted (relative, tritone,
// secondary dominant, borrowed from the parallel key). The same seed gives the same result;
// any amount above 0 changes at least one chord.
Progression varyProgression (const Progression& progression, Key key, double amount, std::uint32_t seed);

// ---- Patterns (Patterns.cpp) ---------------------------------------------------------------
// Data-driven rhythm patterns for the chord and bass parts. A pattern is a string of steps, each
// `stepBeats` long, repeating from the loop start (so it follows the bar lines):
//   chords: 'X' accented hit, 'x' hit, '-' hold the note before, '.' rest
//   bass:   a hit is a letter naming the note, upper case accented: 'r' bass note, 'o' octave
//           above, 'f' fifth, 't' third, 's' seventh (octave if the chord has none), 'w' a walking
//           step towards the next chord, 'a' a chromatic approach note into the next chord;
//           '-' and '.' as for chords.
// Rendering never lets a note cross a chord boundary or the loop end. A note held over a chord
// change is struck again for the new chord; a chord with no hit of its own is struck on its
// start; every bass hit on a chord start or a bar line plays the chord's bass note.
enum class ChordStyle { Block, Rhythmic, Strum, Arpeggio };
enum class ArpOrder { Up, Down, UpDown, Random };
enum class Voicing { Close, Open, Spread };

struct ChordPatternDef
{
    std::string id, name;
    ChordStyle style = ChordStyle::Rhythmic;
    std::string steps;                  // for Block: ignored (one hit per chord)
    double stepBeats = 0.25;
    double gate = 0.9;                  // fraction of a hit's length that sounds
    ArpOrder order = ArpOrder::Up;      // Arpeggio only
    double strumBeats = 0.0;            // Strum only: delay between the notes of a hit
};
struct BassPatternDef
{
    std::string id, name;
    std::string steps;
    double stepBeats = 0.25;
    double gate = 0.9;
    bool slides = false;                // notes that follow another without a gap glide into it
    int octave = 0;                     // added to the normal bass register (C2..B2)
};
const std::vector<ChordPatternDef>& chordPatterns();
const std::vector<BassPatternDef>& bassPatterns();
int findChordPattern (const std::string& id);   // index, or -1
int findBassPattern (const std::string& id);

struct PatternOptions
{
    int pattern = 0;                    // index into chordPatterns() or bassPatterns()
    double arpRate = 0.0;               // beats per arpeggio step, 0 = the pattern's own
    Voicing voicing = Voicing::Close;   // chords only
    int registerCentre = 60;            // chords only: the middle of the voicing (48..76)
    double density = 1.0;               // 0..1: 1 plays the pattern as written, less leaves out weak hits
    double swing = 0.0;                 // 0..1: delays every second step (up to a triplet feel)
    double humanise = 0.0;              // 0..1: small, seeded changes of timing and velocity
    double dynamics = 0.5;              // 0..1: difference between accented and other hits
    int octaveShift = 0;                // -2..2
    std::uint32_t seed = 1;
    double beatsPerBar = 4.0;
};
std::vector<Note> renderChordPattern (const Progression& progression, const PatternOptions& options);
std::vector<Note> renderBassPattern (const Progression& progression, const PatternOptions& options, Key key);
// "Randomise pattern": a seeded pick of pattern, voicing and feel.
PatternOptions randomChordOptions (std::uint32_t seed);
PatternOptions randomBassOptions (std::uint32_t seed);
}
