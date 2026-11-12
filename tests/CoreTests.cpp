#include "Backline.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace backline;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (false)

static const double pi = 3.14159265358979323846;
static const double sampleRate = 44100.0;

static void addTone (std::vector<float>& out, double startSec, double lengthSec, int midi, float gain)
{
    const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
    const auto start = (size_t) (startSec * sampleRate), n = (size_t) (lengthSec * sampleRate);
    for (size_t i = 0; i < n && start + i < out.size(); ++i)
    {
        const double t = (double) i / sampleRate;
        const double env = std::min (1.0, std::min (t * 200.0, (lengthSec - t) * 200.0));
        out[start + i] += gain * (float) (env * (std::sin (2 * pi * f * t) + 0.5 * std::sin (4 * pi * f * t) + 0.25 * std::sin (6 * pi * f * t)));
    }
}

static std::string describe (const std::vector<Chord>& p)
{
    std::string s;
    for (auto c : p) s += chordName (c) + " ";
    return s;
}

static std::string describe (const std::vector<Chord>& p, Key key)
{
    std::string s;
    for (auto c : p) s += chordName (c, key) + " ";
    return s;
}

// A plain sine at any sample rate, with short fades.
static std::vector<float> sine (double rate, double seconds, int midi, float gain)
{
    const double f = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
    std::vector<float> out ((size_t) (rate * seconds));
    for (size_t i = 0; i < out.size(); ++i)
    {
        const double t = (double) i / rate;
        const double env = std::min (1.0, std::min (t * 200.0, (seconds - t) * 200.0));
        out[i] = gain * (float) (env * std::sin (2 * pi * f * t));
    }
    return out;
}

// White noise in [-gain, gain], the same on every platform.
static std::vector<float> noise (size_t numSamples, float gain)
{
    std::vector<float> out (numSamples);
    uint32_t state = 12345u;
    for (auto& v : out)
    {
        state = state * 1664525u + 1013904223u;
        v = gain * ((float) (state >> 8) / 8388608.0f - 1.0f);
    }
    return out;
}

static bool allDifferent (const std::vector<std::vector<Chord>>& ideas)
{
    for (size_t a = 0; a < ideas.size(); ++a)
        for (size_t b = a + 1; b < ideas.size(); ++b)
            if (ideas[a] == ideas[b])
                return false;
    return true;
}

// The one note a take was transcribed as, or -1 if it did not come out as a single note.
static int singleNote (const std::vector<float>& audio, double rate)
{
    const auto notes = audioToNotes (audio.data(), audio.size(), rate, 120.0);
    return notes.size() == 1 ? notes[0].pitch : -1;
}

// ---- Helpers for the progression and tempo tests ----

static std::string describe (const Progression& p, Key key)
{
    std::string s;
    for (const auto& e : p) s += chordName (e.chord, key) + " ";
    return s;
}

static bool hasPitchClass (const std::vector<int>& pitches, int pc)
{
    for (int p : pitches)
        if (((p % 12) + 12) % 12 == ((pc % 12) + 12) % 12)
            return true;
    return false;
}

// Pitches of the chord part sounding at `beat` (notes that start there), lowest first.
static std::vector<int> pitchesAt (const std::vector<Note>& notes, double beat)
{
    std::vector<int> pitches;
    for (const auto& n : notes)
        if (std::abs (n.start - beat) < 1e-9)
            pitches.push_back (n.pitch);
    std::sort (pitches.begin(), pitches.end());
    return pitches;
}

// Every note lies inside the chord it belongs to and every chord is heard.
static bool notesFollowChords (const std::vector<Note>& notes, const Progression& p)
{
    std::vector<bool> heard (p.size(), false);
    for (const auto& n : notes)
    {
        const int index = chordIndexAt (p, n.start);
        if (index < 0 || n.length <= 0.0)
            return false;
        const auto& e = p[(size_t) index];
        if (n.start + n.length > e.start + e.length + 1e-9)
            return false;
        heard[(size_t) index] = true;
    }
    return std::find (heard.begin(), heard.end(), false) == heard.end();
}

static uint32_t nextRandom (uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

// A short burst of decaying noise, like a click or a hi-hat.
static void addClick (std::vector<float>& out, double rate, double startSec, double decaySec, float gain, uint32_t& state)
{
    const auto start = (size_t) (startSec * rate), n = (size_t) (decaySec * 6.0 * rate);
    for (size_t i = 0; i < n && start + i < out.size(); ++i)
        out[start + i] += gain * ((float) nextRandom (state) / 8388608.0f - 1.0f) * (float) std::exp (-(double) i / (decaySec * rate));
}

// A kick: a sine dropping from 120 to 50 Hz.
static void addKick (std::vector<float>& out, double rate, double startSec, float gain)
{
    const auto start = (size_t) (startSec * rate), n = (size_t) (0.25 * rate);
    double phase = 0.0;
    for (size_t i = 0; i < n && start + i < out.size(); ++i)
    {
        const double t = (double) i / rate;
        phase += 2 * pi * (50.0 + 70.0 * std::exp (-t * 30.0)) / rate;
        out[start + i] += gain * (float) (std::sin (phase) * std::exp (-t * 14.0));
    }
}

static std::vector<float> clickTrack (double rate, double seconds, double bpm)
{
    std::vector<float> out ((size_t) (rate * seconds));
    uint32_t state = 777u;
    for (double t = 0.05; t < seconds; t += 60.0 / bpm)
        addClick (out, rate, t, 0.004, 0.6f, state);
    return out;
}

// A drum loop with a bass note: kick on every beat, snare on beats 2 and 4, hats on the eighths.
static std::vector<float> drumLoop (double rate, double seconds, double bpm)
{
    std::vector<float> out ((size_t) (rate * seconds));
    uint32_t state = 4242u;
    const double beat = 60.0 / bpm;
    int index = 0;
    for (double t = 0.02; t < seconds; t += beat, ++index)
    {
        addKick (out, rate, t, 0.5f);
        if (index % 2 == 1) addClick (out, rate, t, 0.03, 0.3f, state);
        addClick (out, rate, t, 0.006, 0.12f, state);
        addClick (out, rate, t + beat * 0.5, 0.006, 0.1f, state);
        const double f = index % 8 < 4 ? 55.0 : 73.42;
        const auto start = (size_t) (t * rate), n = (size_t) (beat * 0.8 * rate);
        for (size_t i = 0; i < n && start + i < out.size(); ++i)
        {
            const double x = (double) i / rate;
            out[start + i] += 0.15f * (float) (std::sin (2 * pi * f * x) * std::min (1.0, std::min (x * 100.0, (beat * 0.8 - x) * 100.0)));
        }
    }
    return out;
}

// A 3/4 drum loop: a kick on the first beat of each bar, a hat on the other two.
static std::vector<float> waltzLoop (double rate, double seconds, double bpm)
{
    std::vector<float> out ((size_t) (rate * seconds));
    uint32_t state = 2u;
    int index = 0;
    for (double t = 0.02; t < seconds; t += 60.0 / bpm, ++index)
    {
        if (index % 3 == 0)
            addKick (out, rate, t, 0.5f);
        else
            addClick (out, rate, t, 0.02, 0.2f, state);
    }
    return out;
}

// Starts of the notes of a part, each once, in order.
static std::vector<double> noteStarts (const std::vector<Note>& notes)
{
    std::vector<double> s;
    for (const auto& n : notes)
        if (s.empty() || s.back() != n.start) s.push_back (n.start);
    return s;
}

// No pitch is struck again before its previous note has ended (MIDI cannot hold the same note twice).
static bool noOverlaps (const std::vector<Note>& notes)
{
    for (size_t a = 0; a < notes.size(); ++a)
        for (size_t b = 0; b < notes.size(); ++b)
            if (a != b && notes[a].pitch == notes[b].pitch && notes[b].start >= notes[a].start
                && notes[b].start < notes[a].start + notes[a].length - 1e-9)
                return false;
    return true;
}

int main()
{
    const double bpm = 120.0, barSeconds = 2.0;

    // 1. Full chords: C - G - Am - F, one bar each.
    {
        std::vector<float> audio ((size_t) (sampleRate * barSeconds * 4));
        const int chords[4][3] = { { 60, 64, 67 }, { 55, 59, 62 }, { 57, 60, 64 }, { 53, 57, 60 } };
        for (int bar = 0; bar < 4; ++bar)
            for (int p : chords[bar])
                addTone (audio, bar * barSeconds, barSeconds, p, 0.15f);

        const auto chroma = computeChroma (audio.data(), audio.size(), sampleRate);
        const auto key = detectKey (chroma);
        std::printf ("chords test: key %s\n", keyName (key).c_str());
        CHECK (key.tonic == 0 && ! key.minor);

        const auto segments = segmentChroma (chroma, bpm, 4.0, 4);
        const auto ideas = suggestProgressions (segments, key, 4);
        CHECK (ideas.size() == 4);
        for (const auto& idea : ideas) std::printf ("  idea: %s\n", describe (idea).c_str());
        CHECK (describe (ideas[0]) == "C G Am F ");
        CHECK (ideas[1] != ideas[0]);
        CHECK (rankChords (segments[2], key).front() == (Chord { 9, Quality::Minor }));
    }

    // 2. Melody only (like a vocal): A minor phrase. Chords must contain the sustained notes.
    {
        std::vector<float> audio ((size_t) (sampleRate * barSeconds * 4));
        const int melody[4][4] = { { 69, 72, 76, 72 }, { 65, 69, 72, 69 }, { 67, 72, 76, 72 }, { 67, 71, 74, 71 } };
        for (int bar = 0; bar < 4; ++bar)
            for (int beat = 0; beat < 4; ++beat)
                addTone (audio, bar * barSeconds + beat * 0.5, 0.45, melody[bar][beat], 0.3f);

        const auto chroma = computeChroma (audio.data(), audio.size(), sampleRate);
        const auto key = detectKey (chroma);
        const auto ideas = suggestProgressions (segmentChroma (chroma, bpm, 4.0, 4), key, 3);
        std::printf ("melody test: key %s, idea: %s\n", keyName (key).c_str(), describe (ideas[0]).c_str());
        CHECK ((key.tonic == 9 && key.minor) || (key.tonic == 0 && ! key.minor));
        // Bar 1 (A C E C) fits both Am and C; either is a valid harmonisation.
        CHECK (describe (ideas[0]) == "Am F C G " || describe (ideas[0]) == "C F C G ");

        const auto notes = audioToNotes (audio.data(), audio.size(), sampleRate, bpm);
        std::printf ("  audio to MIDI: %zu notes\n", notes.size());
        CHECK (notes.size() == 16);
        if (notes.size() == 16)
        {
            CHECK (notes[0].pitch == 69 && notes[1].pitch == 72 && notes[15].pitch == 71);
            CHECK (std::abs (notes[4].start - 4.0) < 0.1);
        }
    }

    // 3. Parts and MIDI export.
    {
        const std::vector<Chord> prog { { 0, Quality::Major }, { 7, Quality::Major } };
        const auto chords = renderChords (prog, 4.0, ChordRhythm::Quarters);
        const auto bass = renderBass (prog, 4.0, BassRhythm::Octaves);
        CHECK (chords.size() == 2 * 4 * 3);
        CHECK (bass.size() == 2 * 8);
        CHECK (bass[0].pitch == 36 && bass[1].pitch == 48 && bass[8].pitch == 43);
        for (const auto& n : chords) CHECK (n.start + n.length <= 8.0 + 1e-9);

        const auto midi = writeMidiFile (chords, bpm);
        CHECK (midi.size() > 22 && std::string (midi.begin(), midi.begin() + 4) == "MThd");
        CHECK (std::string (midi.begin() + 14, midi.begin() + 18) == "MTrk");
        const uint32_t trackLength = (uint32_t) midi[18] << 24 | (uint32_t) midi[19] << 16 | (uint32_t) midi[20] << 8 | midi[21];
        CHECK (trackLength == midi.size() - 22);
    }

    // 4. Chords in the octave below middle C at 44.1 kHz, and a lone C2: the analysis has to
    //    tell neighbouring semitones apart down there at this rate, not only at 48 kHz.
    {
        std::vector<float> audio ((size_t) (sampleRate * barSeconds * 4));
        const int chords[4][3] = { { 48, 52, 55 }, { 43, 47, 50 }, { 45, 48, 52 }, { 41, 45, 48 } };
        for (int bar = 0; bar < 4; ++bar)
            for (int p : chords[bar])
                addTone (audio, bar * barSeconds, barSeconds, p, 0.15f);

        const auto chroma = computeChroma (audio.data(), audio.size(), sampleRate);
        const auto key = detectKey (chroma);
        const auto ideas = suggestProgressions (segmentChroma (chroma, bpm, 4.0, 4), key, 4);
        std::printf ("low chords test: key %s, idea: %s\n", keyName (key).c_str(), describe (ideas[0]).c_str());
        CHECK (key.tonic == 0 && ! key.minor);
        CHECK (describe (ideas[0]) == "C G Am F ");

        const auto c2 = sine (sampleRate, barSeconds, 36, 0.4f);
        const auto segment = segmentChroma (computeChroma (c2.data(), c2.size(), sampleRate), bpm, 4.0, 1)[0];
        CHECK (segment[0] > 0.7f); // C, not C#
    }

    // 5. The same pitch played four times is four notes, however short the gap between them.
    {
        for (double gapSeconds : { 0.01, 0.03, 0.08 })
        {
            std::vector<float> audio ((size_t) (sampleRate * 2.5));
            for (int beat = 0; beat < 4; ++beat)
                addTone (audio, beat * 0.5, 0.5 - gapSeconds, 69, 0.3f);
            const auto notes = audioToNotes (audio.data(), audio.size(), sampleRate, bpm);
            std::printf ("repeated note test (%.0f ms gaps): %zu notes\n", gapSeconds * 1000.0, notes.size());
            CHECK (notes.size() == 4);
            for (size_t k = 0; k < notes.size() && k < 4; ++k)
            {
                CHECK (notes[k].pitch == 69);
                CHECK (std::abs (notes[k].start - (double) k) < 0.15);
            }
        }

        // A held note stays one note, also in the bass where a single cycle is longer than a frame hop.
        for (int pitch : { 69, 40, 28 })
        {
            std::vector<float> audio ((size_t) (sampleRate * 2.5));
            addTone (audio, 0.0, 2.0, pitch, 0.3f);
            const auto notes = audioToNotes (audio.data(), audio.size(), sampleRate, bpm);
            CHECK (notes.size() == 1);
            if (notes.size() == 1)
                CHECK (notes[0].pitch == pitch && notes[0].length > 3.5);
        }
    }

    // 6. Short takes (one or two chords) still get four different ideas.
    {
        auto triad = [] (int root, int third)
        {
            Chroma c {};
            c[(size_t) root] = c[(size_t) ((root + third) % 12)] = c[(size_t) ((root + 7) % 12)] = 1.0f / 3.0f;
            return c;
        };
        const Key cMajor { 0, false, 1.0f };
        const std::vector<Chroma> oneSegment { triad (0, 4) }, twoSegments { triad (0, 4), triad (7, 4) };

        const auto one = suggestProgressions (oneSegment, cMajor, 4);
        const auto two = suggestProgressions (twoSegments, cMajor, 4);
        std::printf ("short take test: %zu and %zu ideas\n", one.size(), two.size());
        CHECK (one.size() == 4 && allDifferent (one));
        CHECK (two.size() == 4 && allDifferent (two));
        if (! one.empty()) CHECK (describe (one[0]) == "C ");
        if (! two.empty()) CHECK (describe (two[0]) == "C G ");

        // Fewer only when the chords run out: one segment in a major key has six possible ideas.
        const auto all = suggestProgressions (oneSegment, cMajor, 10);
        CHECK (all.size() == 6 && allDifferent (all));
        CHECK (suggestProgressions (oneSegment, cMajor, 0).empty());

        // Chroma that was not normalised must not bring the duplicates back.
        auto loud = twoSegments;
        for (auto& segment : loud)
            for (auto& v : segment) v *= 1000.0f;
        const auto fromLoud = suggestProgressions (loud, cMajor, 4);
        CHECK (fromLoud.size() == 4 && allDifferent (fromLoud));
        if (! fromLoud.empty()) CHECK (describe (fromLoud[0]) == "C G ");
    }

    // 7. Audio to MIDI covers E1 to C7: high notes are not written an octave low, bass notes are not lost.
    {
        for (double rate : { 44100.0, 48000.0 })
            for (int pitch : { 28, 33, 84, 88, 93, 96 })
            {
                const int found = singleNote (sine (rate, 1.0, pitch, 0.3f), rate);
                if (found != pitch) std::printf ("range test: MIDI %d at %.0f Hz came out as %d\n", pitch, rate, found);
                CHECK (found == pitch);
            }
    }

    // 8. A quiet take is still transcribed; noise and silence are not.
    {
        std::vector<float> quiet ((size_t) sampleRate);
        addTone (quiet, 0.0, 0.9, 69, 0.01f); // peaks at about 0.015
        CHECK (singleNote (quiet, sampleRate) == 69);

        const auto size = (size_t) (sampleRate * 3.0);
        for (float gain : { 0.5f, 0.02f, 0.002f })
        {
            const auto hiss = noise (size, gain);
            CHECK (audioToNotes (hiss.data(), hiss.size(), sampleRate, bpm).empty());
        }
        const std::vector<float> silence (size);
        CHECK (audioToNotes (silence.data(), silence.size(), sampleRate, bpm).empty());
    }

    // 9. Chords and keys are spelled the way the key is written.
    {
        const Key eMajor { 4, false, 0.0f }, bMajor { 11, false, 0.0f }, fMajor { 5, false, 0.0f };
        const Key cSharpMinor { 1, true, 0.0f }, bFlatMinor { 10, true, 0.0f }, eFlatMinor { 3, true, 0.0f };
        CHECK (describe (chordsInKey (eMajor), eMajor) == "E F#m G#m A B C#m ");
        CHECK (describe (chordsInKey (bMajor), bMajor) == "B C#m D#m E F# G#m ");
        CHECK (describe (chordsInKey (fMajor), fMajor) == "F Gm Am Bb C Dm ");
        CHECK (describe (chordsInKey (cSharpMinor), cSharpMinor) == "C#m E F#m G#m A B G# ");
        CHECK (describe (chordsInKey (bFlatMinor), bFlatMinor) == "Bbm Db Ebm Fm Gb Ab F ");
        CHECK (describe (chordsInKey (eFlatMinor), eFlatMinor) == "Ebm Gb Abm Bbm Cb Db Bb ");
        CHECK (pitchClassName (8, eMajor) == "G#" && pitchClassName (8, fMajor) == "Ab");
        CHECK (pitchClassName (-4, eMajor) == "G#" && pitchClassName (20, eMajor) == "G#");
        // The raised 7th of a minor key (the third of its major V) is a sharpened note, not a flat one.
        CHECK (pitchClassName (8, Key { 9, true, 0.0f }) == "G#" && pitchClassName (1, Key { 2, true, 0.0f }) == "C#");
        CHECK (pitchClassName (6, Key { 7, true, 0.0f }) == "F#" && pitchClassName (11, Key { 0, true, 0.0f }) == "B");
        CHECK (pitchClassName (2, eFlatMinor) == "D" && pitchClassName (4, Key { 5, true, 0.0f }) == "E");

        CHECK (keyName (Key { 6, true, 0.0f }) == "F# minor");
        CHECK (keyName (Key { 10, false, 0.0f }) == "Bb major");
        CHECK (keyName (eFlatMinor) == "Eb minor");
        CHECK (keyName (cSharpMinor) == "C# minor");
        CHECK (keyName (Key { 8, false, 0.0f }) == "Ab major");
        CHECK (keyName (Key { 8, true, 0.0f }) == "G# minor");
        CHECK (keyName (Key { 6, false, 0.0f }) == "F# major");
        CHECK (keyName (Key { 1, false, 0.0f }) == "Db major");

        // In every key each chord root gets its own letter (no "Abm" next to "A").
        for (int tonic = 0; tonic < 12; ++tonic)
            for (bool minor : { false, true })
            {
                const Key key { tonic, minor, 0.0f };
                std::string letters;
                for (auto chord : chordsInKey (key))
                {
                    const char letter = pitchClassName (chord.root, key)[0];
                    if (letters.find (letter) == std::string::npos) letters += letter;
                }
                if (letters.size() != 6) std::printf ("spelling test: %s uses letters %s\n", keyName (key).c_str(), letters.c_str());
                CHECK (letters.size() == 6);
                CHECK (keyName (key).substr (0, keyName (key).find (' ')) == pitchClassName (tonic, key));
            }

        // The forms without a key keep their fixed spelling.
        CHECK (pitchClassName (8) == "Ab" && pitchClassName (1) == "C#");
        CHECK (chordName (Chord { 8, Quality::Minor }) == "Abm");
        CHECK (chordName (Chord { 8, Quality::Minor }, eMajor) == "G#m");
    }

    // 10. Chord qualities: notes, names, spelling, equality.
    {
        const Key cMajor { 0, false, 1.0f }, eMajor { 4, false, 1.0f }, fMinor { 5, true, 1.0f };
        const Quality qualities[] = { Quality::Major, Quality::Minor, Quality::Dominant7, Quality::Major7,
                                      Quality::Minor7, Quality::Sus2, Quality::Sus4, Quality::Diminished };
        const char* const suffixes[] = { "", "m", "7", "maj7", "m7", "sus2", "sus4", "dim" };
        const std::vector<int> intervals[] = { { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 10 }, { 0, 4, 7, 11 },
                                               { 0, 3, 7, 10 }, { 0, 2, 7 }, { 0, 5, 7 }, { 0, 3, 6 } };
        CHECK (numQualities == 8);
        for (int q = 0; q < numQualities; ++q)
        {
            CHECK ((int) qualities[q] == q);
            CHECK (chordIntervals (qualities[q]) == intervals[q]);
            CHECK (qualitySuffix (qualities[q]) == suffixes[q]);
            CHECK (chordName (Chord { 0, qualities[q] }) == std::string ("C") + suffixes[q]);
            CHECK (chordName (Chord { 8, qualities[q] }, eMajor) == std::string ("G#") + suffixes[q]);
            CHECK (chordName (Chord { 8, qualities[q] }, fMinor) == std::string ("Ab") + suffixes[q]);
            CHECK (hasSeventh (qualities[q]) == (intervals[q].size() == 4));
            CHECK (hasMinorThird (qualities[q]) == (intervals[q][1] == 3));
            for (int root = 0; root < 12; ++root)
            {
                const auto pcs = chordPitchClasses (Chord { root, qualities[q] });
                CHECK (pcs.size() == intervals[q].size());
                for (size_t i = 0; i < pcs.size() && i < intervals[q].size(); ++i)
                    CHECK (pcs[i] == (root + intervals[q][i]) % 12);
            }
        }

        // Slash chords.
        CHECK (chordName (Chord { 0, Quality::Major, 4 }) == "C/E");
        CHECK (chordName (Chord { 0, Quality::Major, 7 }) == "C/G");
        CHECK (chordName (Chord { 0, Quality::Major, 0 }) == "C");
        CHECK (chordName (Chord { 0, Quality::Major, -1 }) == "C");
        CHECK (chordName (Chord { 9, Quality::Minor7, 7 }) == "Am7/G");
        CHECK (chordName (Chord { 4, Quality::Major, 8 }, eMajor) == "E/G#");
        CHECK (chordName (Chord { 1, Quality::Major, 8 }, fMinor) == "Db/Ab");
        CHECK (chordName (Chord { 2, Quality::Dominant7, 6 }, cMajor) == "D7/F#");
        CHECK ((Chord { 0, Quality::Major, 0 }) == (Chord { 0, Quality::Major }));
        CHECK ((Chord { 0, Quality::Major, 4 }) != (Chord { 0, Quality::Major }));
        CHECK ((Chord { 0, Quality::Major }) != (Chord { 0, Quality::Major7 }));
        CHECK ((Chord { 0, Quality::Major, 4 }).bassPitchClass() == 4 && (Chord { 7, Quality::Minor }).bassPitchClass() == 7);

        CHECK (isInScale (11, cMajor) && ! isInScale (10, cMajor) && isInScale (-1, cMajor));
        CHECK (isInScale (8, fMinor) && ! isInScale (4, fMinor)); // Ab but not E natural

        // Ranking still returns the key's triads, best first.
        Chroma c {};
        c[9] = c[0] = c[4] = 1.0f / 3.0f;
        CHECK (rankChords (c, cMajor).front() == (Chord { 9, Quality::Minor }));
        CHECK (rankChords (c, cMajor).size() == 6);
    }

    // 11. Progressions as chord events.
    {
        const Key cMajor { 0, false, 1.0f };
        const std::vector<Chord> grid { { 0, Quality::Major }, { 7, Quality::Major }, { 9, Quality::Minor }, { 5, Quality::Major } };
        for (double beatsPerChord : { 2.0, 4.0, 8.0, 1.5, 3.0 })
        {
            const auto p = toProgression (grid, beatsPerChord);
            CHECK (p.size() == 4 && isValidProgression (p));
            CHECK (progressionLength (p) == 4.0 * beatsPerChord);
            CHECK (p[2].start == 2.0 * beatsPerChord && p[2].length == beatsPerChord);
            CHECK (chordsOf (p) == grid);
            CHECK (chordIndexAt (p, 0.0) == 0 && chordIndexAt (p, beatsPerChord) == 1);
            CHECK (chordIndexAt (p, 4.0 * beatsPerChord - 0.01) == 3);
            CHECK (chordIndexAt (p, 4.0 * beatsPerChord) == -1 && chordIndexAt (p, -0.5) == -1);
        }
        CHECK (! isValidProgression (Progression {}));
        CHECK (progressionLength (Progression {}) == 0.0);
        CHECK (! isValidProgression (Progression { { grid[0], 1.0, 4.0 } }));                          // does not start at 0
        CHECK (! isValidProgression (Progression { { grid[0], 0.0, 4.0 }, { grid[1], 5.0, 3.0 } }));   // gap
        CHECK (! isValidProgression (Progression { { grid[0], 0.0, 4.0 }, { grid[1], 3.0, 5.0 } }));   // overlap
        CHECK (! isValidProgression (Progression { { grid[0], 0.0, 0.0 } }));                          // empty chord

        const auto p = toProgression (grid, 4.0);

        // Replace.
        const auto replaced = setChord (p, 1, { 4, Quality::Minor7 });
        CHECK (describe (replaced, cMajor) == "C Em7 Am F " && replaced[1].start == 4.0 && replaced[1].length == 4.0);
        CHECK (setChord (p, 9, { 4, Quality::Minor }) == p);
        CHECK (describe (p, cMajor) == "C G Am F "); // the argument is not touched

        // Split.
        const auto split = splitChord (p, 1, 1.0);
        CHECK (split.size() == 5 && isValidProgression (split) && progressionLength (split) == 16.0);
        CHECK (describe (split, cMajor) == "C G G Am F ");
        CHECK (split[1].start == 4.0 && split[1].length == 2.0 && split[2].start == 6.0 && split[2].length == 2.0);
        const auto three = toProgression (grid, 3.0);
        const auto uneven = splitChord (three, 0, 1.0); // 3 beats on a 1 beat grid: 2 + 1
        CHECK (uneven.size() == 5 && uneven[0].length == 2.0 && uneven[1].length == 1.0 && isValidProgression (uneven));
        CHECK (splitChord (three, 0, 0.0)[0].length == 1.5);          // no grid: exact middle
        CHECK (splitChord (three, 0, 2.0) == three);                  // no room for two grid steps
        CHECK (splitChord (toProgression (grid, 1.0), 2, 0.5)[2].length == 0.5);
        CHECK (splitChord (toProgression (grid, 1.0), 2, 1.0) == toProgression (grid, 1.0));
        CHECK (splitChord (p, 4, 1.0) == p);
        const auto twice = splitChord (splitChord (p, 3, 1.0), 4, 1.0); // F: 2 + 1 + 1
        CHECK (twice.size() == 6 && twice[4].length == 1.0 && twice[5].length == 1.0 && isValidProgression (twice));

        // Move a boundary.
        auto moved = moveBoundary (p, 0, 6.0, 1.0);
        CHECK (isValidProgression (moved) && moved[0].length == 6.0 && moved[1].start == 6.0 && moved[1].length == 2.0);
        CHECK (moved[2] == p[2] && progressionLength (moved) == 16.0);
        moved = moveBoundary (p, 0, 2.6, 1.0);   // snaps to 3
        CHECK (moved[0].length == 3.0 && moved[1].length == 5.0);
        moved = moveBoundary (p, 0, 2.6, 0.5);   // snaps to 2.5
        CHECK (moved[0].length == 2.5 && moved[1].length == 5.5);
        moved = moveBoundary (p, 0, -3.0, 1.0);  // the first chord keeps one grid step
        CHECK (moved[0].length == 1.0 && moved[1].length == 7.0 && isValidProgression (moved));
        moved = moveBoundary (p, 0, 99.0, 1.0);  // so does the second
        CHECK (moved[0].length == 7.0 && moved[1].length == 1.0 && isValidProgression (moved));
        moved = moveBoundary (p, 2, 11.9, 2.0);
        CHECK (moved[2].length == 4.0 && moved[3].length == 4.0);
        moved = moveBoundary (p, 2, 13.1, 2.0);
        CHECK (moved[2].length == 6.0 && moved[3].length == 2.0);
        moved = moveBoundary (p, 1, 9.3, 0.0);   // no grid
        CHECK (std::abs (moved[1].length - 5.3) < 1e-9 && isValidProgression (moved) && std::abs (progressionLength (moved) - 16.0) < 1e-9);
        CHECK (moveBoundary (p, 3, 14.0, 1.0) == p); // there is no boundary after the last chord
        CHECK (moveBoundary (p, 7, 14.0, 1.0) == p);
        CHECK (moveBoundary (toProgression (grid, 1.0), 0, 0.5, 1.0) == toProgression (grid, 1.0)); // nowhere to go
        // Chords that do not start on the grid still keep a full grid step each.
        const Progression offGrid { { grid[0], 0.0, 0.5 }, { grid[1], 0.5, 3.0 }, { grid[2], 3.5, 2.5 } };
        moved = moveBoundary (offGrid, 1, 0.0, 1.0);
        CHECK (isValidProgression (moved) && moved[1].length >= 1.0 - 1e-9 && moved[2].length >= 1.0 - 1e-9 && moved[1].start == 0.5);
        CHECK (moved[2].start == 2.0);
        moved = moveBoundary (offGrid, 1, 9.0, 1.0);
        CHECK (isValidProgression (moved) && moved[2].start == 5.0 && progressionLength (moved) == 6.0);

        // Delete.
        auto deleted = deleteChord (p, 2);
        CHECK (describe (deleted, cMajor) == "C G F " && isValidProgression (deleted) && progressionLength (deleted) == 16.0);
        CHECK (deleted[1].start == 4.0 && deleted[1].length == 8.0 && deleted[2].start == 12.0);
        deleted = deleteChord (p, 0);
        CHECK (describe (deleted, cMajor) == "G Am F " && deleted[0].start == 0.0 && deleted[0].length == 8.0 && isValidProgression (deleted));
        deleted = deleteChord (p, 3);
        CHECK (describe (deleted, cMajor) == "C G Am " && deleted[2].length == 8.0 && progressionLength (deleted) == 16.0);
        CHECK (deleteChord (p, 4) == p);
        auto last = p;
        for (int i = 0; i < 10; ++i)
            last = deleteChord (last, 0);
        CHECK (last.size() == 1 && describe (last, cMajor) == "F " && last[0].start == 0.0 && last[0].length == 16.0);
        CHECK (deleteChord (last, 0) == last);
    }

    // 12. Passing chords.
    {
        const Key cMajor { 0, false, 1.0f }, aMinor { 9, true, 1.0f };
        const Chord C { 0, Quality::Major }, Dm { 2, Quality::Minor }, Em { 4, Quality::Minor }, F { 5, Quality::Major },
                    G { 7, Quality::Major }, Am { 9, Quality::Minor }, G7 { 7, Quality::Dominant7 };
        CHECK (chordName (passingChord (C, F, cMajor), cMajor) == "C7");      // secondary dominant
        CHECK (chordName (passingChord (C, Am, cMajor), cMajor) == "G/B");    // bass walks C B A
        CHECK (chordName (passingChord (F, Dm, cMajor), cMajor) == "C/E");
        CHECK (chordName (passingChord (C, Em, cMajor), cMajor) == "Dm");     // diatonic step between
        CHECK (chordName (passingChord (C, Dm, cMajor), cMajor) == "C#dim");  // chromatic approach
        CHECK (chordName (passingChord (F, G, cMajor), cMajor) == "F#dim");
        CHECK (chordName (passingChord (Am, F, cMajor), cMajor) == "Em/G");
        CHECK (chordName (passingChord (F, C, cMajor), cMajor) == "G7");
        CHECK (chordName (passingChord (G, C, cMajor), cMajor) == "G7");
        CHECK (chordName (passingChord (G7, C, cMajor), cMajor) == "Bdim");   // already on the dominant
        CHECK (chordName (passingChord (C, C, cMajor), cMajor) == "G7");
        CHECK (chordName (passingChord (Am, Dm, aMinor), aMinor) == "A7");
        CHECK (chordName (passingChord (Dm, Am, aMinor), aMinor) == "E7");
        CHECK (chordName (passingChord (Am, C, aMinor), aMinor) == "G7");     // no triad on B in the key

        // Leading-note diminished chords lead up, so they are spelled with a sharp in flat keys too.
        const Key fMajor { 5, false, 1.0f }, bbMajor { 10, false, 1.0f }, ebMajor { 3, false, 1.0f }, dMinor { 2, true, 1.0f };
        const Chord Gm { 7, Quality::Minor }, Bb { 10, Quality::Major }, Eb { 3, Quality::Major }, A { 9, Quality::Major };
        CHECK (chordName (passingChord (F, Gm, fMajor), fMajor) == "F#dim");
        CHECK (chordName (passingChord (Gm, Am, fMajor), fMajor) == "G#dim");
        CHECK (chordName (passingChord ({ 0, Quality::Major }, Dm, fMajor), fMajor) == "C#dim");
        CHECK (chordName (passingChord (F, Gm, bbMajor), bbMajor) == "F#dim");
        CHECK (chordName (passingChord (F, Gm, ebMajor), ebMajor) == "F#dim");
        CHECK (chordName (passingChord (Gm, Am, dMinor), dMinor) == "G#dim");
        CHECK (chordName (passingChord (C, Dm, cMajor), cMajor) == "C#dim");
        CHECK (chordName (passingChord (Bb, C, fMajor), fMajor) == "Bdim");      // a scale note keeps the key's spelling
        CHECK (chordName (passingChord (Eb, F, bbMajor), bbMajor) == "Edim");
        CHECK (chordName (passingChord ({ 0, Quality::Dominant7 }, F, fMajor), fMajor) == "Edim"); // already on the dominant
        CHECK (chordName (passingChord (A, Bb, fMajor), fMajor) == "F7");         // not a diminished chord: no hint
        CHECK (chordName (passingChord (F, Gm, fMajor)) == "F#dim");             // the neutral spelling agrees
        CHECK (chordName (Chord { 6, Quality::Diminished }, fMajor) == "Gbdim");  // without the hint, the key decides
        // The hint is only spelling: the chord is the same chord, and its variations are spelled alike.
        CHECK (passingChord (F, Gm, fMajor) == (Chord { 6, Quality::Diminished }));
        CHECK (chordName (chordVariations (passingChord (F, Gm, fMajor), fMajor).front(), fMajor) == "F#");

        // Whatever the two chords and the key, the passing chord is a third chord that leads on:
        // it is a dominant or diminished chord resolving to the target, or it belongs to the key.
        for (int tonic = 0; tonic < 12; ++tonic)
            for (bool minor : { false, true })
            {
                const Key key { tonic, minor, 1.0f };
                auto chords = chordsInKey (key);
                chords.push_back ({ (tonic + 7) % 12, Quality::Dominant7 });
                chords.push_back ({ (tonic + 1) % 12, Quality::Major7, (tonic + 5) % 12 });
                for (auto from : chords)
                    for (auto to : chords)
                    {
                        const Chord pass = passingChord (from, to, key);
                        const bool sameAsNeighbour = (pass.root == from.root && pass.quality == from.quality)
                                                  || (pass.root == to.root && pass.quality == to.quality);
                        CHECK (! sameAsNeighbour);
                        CHECK (pass.root >= 0 && pass.root < 12 && pass.bass >= -1 && pass.bass < 12);
                        const bool dominant = pass.quality == Quality::Dominant7 && pass.root == (to.root + 7) % 12;
                        const bool leadingNote = pass.quality == Quality::Diminished
                                              && (pass.root == (to.root + 11) % 12 || pass.root == (from.root + 1) % 12);
                        const auto inKey = chordsInKey (key);
                        const bool diatonic = std::find (inKey.begin(), inKey.end(), Chord { pass.root, pass.quality }) != inKey.end();
                        CHECK (dominant || leadingNote || diatonic);
                        if (pass.bass >= 0)
                            CHECK (hasPitchClass (chordPitchClasses (pass), pass.bass));
                    }
            }

        // Inserting: the passing chord takes the last beat, or the second half of a short chord.
        const auto p = toProgression ({ C, G, Am, F }, 4.0);
        auto withPass = insertPassingChord (p, 0, cMajor);
        CHECK (describe (withPass, cMajor) == "C D7 G Am F " && isValidProgression (withPass) && progressionLength (withPass) == 16.0);
        CHECK (withPass[0].length == 3.0 && withPass[1].start == 3.0 && withPass[1].length == 1.0 && withPass[2].start == 4.0);
        withPass = insertPassingChord (p, 3, cMajor); // the last chord leads back to the first
        CHECK (describe (withPass, cMajor) == "C G Am F G7 " && withPass[4].start == 15.0 && withPass[4].length == 1.0);
        CHECK (isValidProgression (withPass) && progressionLength (withPass) == 16.0);
        withPass = insertPassingChord (toProgression ({ C, Am }, 2.0), 0, cMajor);
        CHECK (describe (withPass, cMajor) == "C G/B Am " && withPass[0].length == 1.0 && withPass[1].length == 1.0);
        withPass = insertPassingChord (toProgression ({ C, Am }, 1.5), 0, cMajor);
        CHECK (withPass.size() == 3 && withPass[0].length == 0.75 && withPass[1].start == 0.75 && withPass[1].length == 0.75);
        withPass = insertPassingChord (toProgression ({ C, Am }, 1.0), 1, cMajor);
        CHECK (withPass.size() == 3 && withPass[2].start == 1.5 && withPass[2].length == 0.5 && isValidProgression (withPass));
        CHECK (insertPassingChord (toProgression ({ C, Am }, 0.125), 0, cMajor).size() == 2); // too short
        CHECK (insertPassingChord (p, 4, cMajor) == p);
        const auto alone = insertPassingChord (toProgression ({ C }, 4.0), 0, cMajor); // one chord loops into itself
        CHECK (describe (alone, cMajor) == "C G7 " && isValidProgression (alone));
    }

    // 13. Any sequence of edits leaves a contiguous progression of the same length.
    {
        uint32_t state = 2024u;
        for (int tonic = 0; tonic < 12; ++tonic)
        {
            const Key key { tonic, (tonic % 2) == 1, 1.0f };
            const double beatsPerChord = tonic % 3 == 0 ? 2.0 : tonic % 3 == 1 ? 4.0 : 1.5;
            Progression p = toProgression (suggestProgressions (std::vector<Chroma> (4, Chroma {}), key, 1)[0], beatsPerChord);
            const double length = progressionLength (p);
            bool ok = isValidProgression (p);
            for (int step = 0; step < 400 && ok; ++step)
            {
                const size_t index = (size_t) (nextRandom (state) % (uint32_t) (p.size() + 1)); // sometimes out of range
                const double grid = (nextRandom (state) % 3u == 0u) ? 0.5 : 1.0;
                const size_t before = p.size();
                switch (nextRandom (state) % 7u)
                {
                    case 0: p = splitChord (p, index, grid); ok = p.size() == before || p.size() == before + 1; break;
                    case 1: p = moveBoundary (p, index, length * (double) (nextRandom (state) % 1000u) / 900.0 - 1.0, grid); ok = p.size() == before; break;
                    case 2: p = deleteChord (p, index); ok = p.size() == before || p.size() + 1 == before; break;
                    case 3: p = insertPassingChord (p, index, key); ok = p.size() == before || p.size() == before + 1; break;
                    case 4:
                    {
                        const auto variations = index < p.size() ? chordVariations (p[index].chord, key) : std::vector<Chord>();
                        if (! variations.empty())
                            p = setChord (p, index, variations[nextRandom (state) % (uint32_t) variations.size()]);
                        break;
                    }
                    case 5: p = withSevenths (p, key); break;
                    default: p = withoutSevenths (p); break;
                }
                ok = ok && ! p.empty() && isValidProgression (p) && std::abs (progressionLength (p) - length) < 1e-9;
                if (p.size() > 24) // keep it from only growing
                    p = deleteChord (deleteChord (p, 0), p.size() / 2);

                // Whatever it has become, it renders: every note inside its chord, every chord heard.
                if (ok && step % 20 == 0)
                {
                    for (auto r : { ChordRhythm::Sustain, ChordRhythm::Quarters, ChordRhythm::Offbeat, ChordRhythm::Dotted })
                        ok = ok && notesFollowChords (renderChords (p, r), p);
                    for (auto r : { BassRhythm::Sustain, BassRhythm::Eighths, BassRhythm::Octaves, BassRhythm::RootFifth, BassRhythm::Offbeat })
                        ok = ok && notesFollowChords (renderBass (p, r), p);
                }
            }
            if (! ok) std::printf ("edit sequence test failed in %s: %s\n", keyName (key).c_str(), describe (p, key).c_str());
            CHECK (ok);
        }
    }

    // 14. Variations of one chord.
    {
        const Key cMajor { 0, false, 1.0f }, aMinor { 9, true, 1.0f };
        auto names = [] (const std::vector<Chord>& chords, Key key)
        {
            std::string s;
            for (auto c : chords) s += chordName (c, key) + " ";
            return s;
        };
        CHECK (names (chordVariations ({ 0, Quality::Major }, cMajor), cMajor) == "Cmaj7 Csus2 Csus4 C/E C/G ");
        CHECK (names (chordVariations ({ 7, Quality::Major }, cMajor), cMajor) == "G7 Gsus2 Gsus4 G/B G/D ");
        CHECK (names (chordVariations ({ 2, Quality::Minor }, cMajor), cMajor) == "Dm7 Dsus2 Dsus4 Dm/F Dm/A ");
        CHECK (names (chordVariations ({ 4, Quality::Minor }, cMajor), cMajor) == "Em7 Esus4 Em/G Em/B ");       // no F# for Esus2
        CHECK (names (chordVariations ({ 5, Quality::Major }, cMajor), cMajor) == "Fmaj7 Fsus2 F/A F/C ");       // no Bb for Fsus4
        CHECK (names (chordVariations ({ 11, Quality::Diminished }, cMajor), cMajor) == "Bdim/D Bdim/F ");
        CHECK (names (chordVariations ({ 0, Quality::Major7 }, cMajor), cMajor) == "C Csus2 Csus4 Cmaj7/E Cmaj7/G ");
        CHECK (names (chordVariations ({ 0, Quality::Major, 4 }, cMajor), cMajor) == "C Cmaj7 Csus2 Csus4 C/G "); // from an inversion back to the root
        CHECK (names (chordVariations ({ 4, Quality::Major }, aMinor), aMinor) == "Em E7 Em7 Esus4 E/G# E/B ");   // the major V of a minor key
        CHECK (names (chordVariations ({ 9, Quality::Minor }, aMinor), aMinor) == "Am7 Asus2 Asus4 Am/C Am/E ");
        // A chord from outside the key (a passing chord) can become anything.
        CHECK (names (chordVariations ({ 1, Quality::Diminished }, cMajor), cMajor) == "C# C#m C#7 C#maj7 C#m7 C#sus2 C#sus4 C#dim/E C#dim/G ");
        // A chord on a note of the key whose own quality is foreign (D7 in C) keeps that quality on offer.
        CHECK (names (chordVariations ({ 2, Quality::Dominant7, 6 }, cMajor), cMajor) == "Dm D7 Dm7 Dsus2 Dsus4 D7/A ");

        for (int tonic = 0; tonic < 12; ++tonic)
            for (bool minor : { false, true })
            {
                const Key key { tonic, minor, 1.0f };
                for (auto chord : chordsInKey (key))
                {
                    const auto variations = chordVariations (chord, key);
                    CHECK (variations.size() >= 3);
                    CHECK (std::find (variations.begin(), variations.end(), chord) == variations.end());
                    int inversions = 0;
                    for (size_t i = 0; i < variations.size(); ++i)
                    {
                        const Chord v = variations[i];
                        CHECK (v.root == chord.root);
                        for (size_t j = i + 1; j < variations.size(); ++j)
                            CHECK (v != variations[j]);
                        if (v.bassPitchClass() != v.root)
                        {
                            ++inversions;
                            CHECK (v.quality == chord.quality && hasPitchClass (chordPitchClasses (v), v.bass));
                        }
                        else
                            for (int pc : chordPitchClasses (v)) // every note is in the key (or its raised 7th in minor)
                                CHECK (isInScale (pc, key) || (minor && pc == (tonic + 11) % 12));
                    }
                    CHECK (inversions == 2);
                }
            }
    }

    // 15. Sevenths colouring.
    {
        const Key cMajor { 0, false, 1.0f }, aMinor { 9, true, 1.0f }, eMajor { 4, false, 1.0f };
        auto coloured = [] (Key key)
        {
            std::string s;
            for (auto c : chordsInKey (key)) s += chordName (seventhChord (c, key), key) + " ";
            return s;
        };
        CHECK (coloured (cMajor) == "Cmaj7 Dm7 Em7 Fmaj7 G7 Am7 ");
        CHECK (coloured (eMajor) == "Emaj7 F#m7 G#m7 Amaj7 B7 C#m7 ");
        CHECK (coloured (aMinor) == "Am7 Cmaj7 Dm7 Em7 Fmaj7 G7 E7 ");

        const auto p = toProgression ({ { 0, Quality::Major }, { 7, Quality::Major, 11 }, { 9, Quality::Minor }, { 5, Quality::Sus2 },
                                        { 2, Quality::Major }, { 1, Quality::Diminished }, { 6, Quality::Major } }, 2.0);
        const auto sevenths = withSevenths (p, cMajor);
        // G/B keeps its bass; Fsus2 and C#dim are not triads; D major gets the key's C; F# is outside the key.
        CHECK (describe (sevenths, cMajor) == "Cmaj7 G7/B Am7 Fsus2 D7 C#dim F# ");
        CHECK (isValidProgression (sevenths) && sevenths.size() == p.size() && sevenths[3].start == p[3].start);
        CHECK (withSevenths (sevenths, cMajor) == sevenths);   // applying it twice changes nothing more
        CHECK (withoutSevenths (sevenths) == p);
        CHECK (withoutSevenths (p) == p);
        CHECK (triadChord ({ 9, Quality::Minor7, 7 }) == (Chord { 9, Quality::Minor }));      // Am7/G has no G left to stand on
        CHECK (triadChord ({ 9, Quality::Minor7, 0 }) == (Chord { 9, Quality::Minor, 0 }));   // Am7/C stays Am/C
        CHECK (triadChord ({ 0, Quality::Major7 }) == (Chord { 0, Quality::Major }));
        CHECK (triadChord ({ 0, Quality::Sus4 }) == (Chord { 0, Quality::Sus4 }));

        // Only the 7ths the colouring added are taken back. A passing dominant stays a dominant...
        const Chord C { 0, Quality::Major }, F { 5, Quality::Major }, G { 7, Quality::Major }, Am { 9, Quality::Minor };
        const auto withPass = insertPassingChord (toProgression ({ C, F }, 4.0), 0, cMajor);
        CHECK (describe (withPass, cMajor) == "C C7 F " && ! withPass[1].coloured);
        CHECK (describe (withSevenths (withPass, cMajor), cMajor) == "Cmaj7 C7 Fmaj7 ");
        CHECK (describe (withoutSevenths (withSevenths (withPass, cMajor)), cMajor) == "C C7 F ");
        CHECK (withoutSevenths (withSevenths (withPass, cMajor)) == withPass);
        // ...and so does a 7th chord the user picked, before or while the colouring is on.
        const auto picked = setChord (toProgression ({ C, Am, F, G }, 4.0), 1, { 9, Quality::Minor7 });
        CHECK (describe (withSevenths (picked, cMajor), cMajor) == "Cmaj7 Am7 Fmaj7 G7 ");
        CHECK (describe (withoutSevenths (withSevenths (picked, cMajor)), cMajor) == "C Am7 F G ");
        auto on = withSevenths (toProgression ({ C, Am, F, G }, 4.0), cMajor);
        CHECK (on[1].coloured && on[3].coloured);
        on = setChord (on, 3, { 7, Quality::Dominant7, 11 });                   // G7/B, picked from the menu
        CHECK (! on[3].coloured && describe (withoutSevenths (on), cMajor) == "C Am F G7/B ");
        // Splitting, moving, deleting and inserting keep the marks of the chords they move.
        on = withSevenths (toProgression ({ C, Am, F, G }, 4.0), cMajor);
        on = splitChord (on, 0, 1.0);
        on = moveBoundary (on, 2, 9.0, 1.0);
        on = deleteChord (on, 3);
        on = insertPassingChord (on, 2, cMajor);
        std::printf ("coloured, edited: %s\n", describe (on, cMajor).c_str());
        CHECK (describe (on, cMajor) == "Cmaj7 Cmaj7 Am7 D7 G7 ");
        CHECK (describe (withoutSevenths (on), cMajor) == "C C Am D7 G ");
        CHECK (on[3].start == 11.0 && on[3].length == 1.0 && on[2].start == 4.0 && on[2].length == 7.0);
        CHECK (withSevenths (withSevenths (on, cMajor), cMajor) == withSevenths (on, cMajor));
    }

    // 16. Voicings and bass for every quality, inversion and rhythm.
    {
        const ChordRhythm chordRhythms[] = { ChordRhythm::Sustain, ChordRhythm::Quarters, ChordRhythm::Offbeat, ChordRhythm::Dotted };
        const BassRhythm bassRhythms[] = { BassRhythm::Sustain, BassRhythm::Eighths, BassRhythm::Octaves, BassRhythm::RootFifth, BassRhythm::Offbeat };

        for (int q = 0; q < numQualities; ++q)
            for (int root = 0; root < 12; ++root)
            {
                const Chord plain { root, (Quality) q };
                const auto tones = chordPitchClasses (plain);
                std::vector<Chord> forms { plain };
                for (size_t i = 1; i < tones.size(); ++i)
                    forms.push_back ({ root, (Quality) q, tones[i] });      // every inversion
                forms.push_back ({ root, (Quality) q, (root + 1) % 12 });   // and a bass note from outside the chord

                for (auto chord : forms)
                {
                    const bool foreignBass = ! hasPitchClass (tones, chord.bassPitchClass());
                    // Second in a progression, so the voicing has a previous chord to stay close to.
                    const Progression p { { { (root + 5) % 12, Quality::Major }, 0.0, 4.0 }, { chord, 4.0, 4.0 } };
                    for (auto rhythm : chordRhythms)
                    {
                        const auto notes = renderChords (p, rhythm);
                        CHECK (notesFollowChords (notes, p));
                        double firstHit = 99.0;
                        for (const auto& n : notes)
                            if (n.start >= 4.0) firstHit = std::min (firstHit, n.start);
                        const auto pitches = pitchesAt (notes, firstHit);
                        CHECK (pitches.size() == tones.size() + (foreignBass ? 1u : 0u));  // 7th chords have four notes
                        for (int pc : tones)
                            CHECK (hasPitchClass (pitches, pc));                           // every chord tone, the 7th included
                        for (int pitch : pitches)
                            CHECK (hasPitchClass (tones, pitch) || pitch % 12 == chord.bassPitchClass());
                        if (! pitches.empty())
                        {
                            if (chord.bass >= 0)
                                CHECK (pitches.front() % 12 == chord.bassPitchClass());    // the bass note is the lowest voice
                            CHECK (pitches.front() >= 36 && pitches.back() <= 79);
                            CHECK (pitches.back() - pitches.front() <= (chord.bass >= 0 ? 23 : 11));
                        }
                    }
                    for (auto rhythm : bassRhythms)
                    {
                        const auto notes = renderBass (p, rhythm);
                        CHECK (notesFollowChords (notes, p));
                        bool first = true;
                        for (const auto& n : notes)
                        {
                            if (n.start < 4.0)
                                continue;
                            CHECK (n.pitch >= 36 && n.pitch <= 59);
                            if (first || rhythm != BassRhythm::RootFifth)
                                CHECK (n.pitch % 12 == chord.bassPitchClass());            // the bass plays the bass note
                            else
                                CHECK (n.pitch % 12 == chord.bassPitchClass() || hasPitchClass (tones, n.pitch)); // or a chord tone
                            first = false;
                        }
                        CHECK (! first);
                    }
                }
            }

        // Exact voicings: close position around middle C, slash bass underneath.
        const Progression p { { { 0, Quality::Major7 }, 0.0, 4.0 }, { { 0, Quality::Major, 4 }, 4.0, 2.0 },
                              { { 7, Quality::Dominant7, 11 }, 6.0, 2.0 }, { { 11, Quality::Diminished }, 8.0, 1.0 } };
        const auto chords = renderChords (p, ChordRhythm::Sustain);
        CHECK (pitchesAt (chords, 0.0) == (std::vector<int> { 55, 59, 60, 64 }));   // G B C E
        CHECK (pitchesAt (chords, 4.0) == (std::vector<int> { 52, 55, 60 }));       // E G C
        CHECK (pitchesAt (chords, 6.0) == (std::vector<int> { 47, 50, 53, 55 }));   // B D F G
        CHECK (pitchesAt (chords, 8.0).size() == 3);
        CHECK (chords.size() == 4 + 3 + 4 + 3);
        for (const auto& n : chords)
            CHECK (n.length == p[(size_t) chordIndexAt (p, n.start)].length);

        // Bass: slash chords play their bass note; root and fifth alternates with the root above it.
        auto bass = renderBass (p, BassRhythm::RootFifth);
        CHECK (bass.size() == 4 + 2 + 2 + 1);
        CHECK (bass[0].pitch == 36 && bass[1].pitch == 43);                         // C, G
        CHECK (bass[4].pitch == 40 && bass[5].pitch == 48);                         // E, C above
        CHECK (bass[6].pitch == 47 && bass[7].pitch == 55);                         // B, G above
        CHECK (bass[8].pitch == 47);
        const Progression dim { { { 11, Quality::Diminished }, 0.0, 2.0 } };
        bass = renderBass (dim, BassRhythm::RootFifth);
        CHECK (bass.size() == 2 && bass[0].pitch == 47 && bass[1].pitch == 53);     // B and its diminished fifth F
        bass = renderBass (p, BassRhythm::Octaves);
        CHECK (bass[8].pitch == 40 && bass[9].pitch == 52 && bass[8].start == 4.0 && bass[9].start == 4.5);

        // Chords of different lengths: the rhythm starts again with each chord (the dotted figure
        // follows the bar lines instead, and strikes a chord that starts between its hits).
        const Progression uneven { { { 0, Quality::Major }, 0.0, 3.0 }, { { 9, Quality::Minor }, 3.0, 1.0 }, { { 5, Quality::Major }, 4.0, 0.5 },
                                   { { 7, Quality::Major }, 4.5, 3.5 } };
        auto starts = [] (const std::vector<Note>& notes)
        {
            std::vector<double> s;
            for (const auto& n : notes)
                if (s.empty() || s.back() != n.start) s.push_back (n.start);
            return s;
        };
        CHECK (starts (renderChords (uneven, ChordRhythm::Sustain)) == (std::vector<double> { 0.0, 3.0, 4.0, 4.5 }));
        CHECK (starts (renderChords (uneven, ChordRhythm::Quarters)) == (std::vector<double> { 0.0, 1.0, 2.0, 3.0, 4.0, 4.5, 5.5, 6.5, 7.5 }));
        // The half-beat chord has no offbeat of its own, so it is played on its start.
        CHECK (starts (renderChords (uneven, ChordRhythm::Offbeat)) == (std::vector<double> { 0.5, 1.5, 2.5, 3.5, 4.0, 5.0, 6.0, 7.0 }));
        CHECK (starts (renderChords (uneven, ChordRhythm::Dotted)) == (std::vector<double> { 0.0, 1.5, 3.0, 4.0, 4.5, 5.5, 7.0 }));
        CHECK (starts (renderBass (uneven, BassRhythm::Offbeat)) == (std::vector<double> { 0.5, 1.5, 2.5, 3.5, 4.0, 5.0, 6.0, 7.0 }));
        CHECK (starts (renderBass (uneven, BassRhythm::Eighths)).size() == 16);
        for (auto r : chordRhythms) CHECK (notesFollowChords (renderChords (uneven, r), uneven) && noOverlaps (renderChords (uneven, r)));

        // The dotted figure keeps to the bar: in 3/4 a six-beat chord is struck on 0, 1.5, 3 and 4.5,
        // and so is the next one from its own bar line on.
        const Chord C { 0, Quality::Major }, G { 7, Quality::Major };
        const Progression waltz { { C, 0.0, 6.0 }, { G, 6.0, 6.0 } };
        CHECK (noteStarts (renderChords (Progression { { C, 0.0, 6.0 } }, ChordRhythm::Dotted, 3.0)) == (std::vector<double> { 0.0, 1.5, 3.0, 4.5 }));
        CHECK (noteStarts (renderChords (waltz, ChordRhythm::Dotted, 3.0)) == (std::vector<double> { 0.0, 1.5, 3.0, 4.5, 6.0, 7.5, 9.0, 10.5 }));
        // In 4/4 a chord change moved to beat 5 does not shift the second bar's figure.
        const Progression resized { { C, 0.0, 5.0 }, { G, 5.0, 3.0 } };
        CHECK (noteStarts (renderChords (resized, ChordRhythm::Dotted)) == (std::vector<double> { 0.0, 1.5, 3.0, 4.0, 5.0, 5.5, 7.0 }));
        CHECK (noteStarts (renderChords (resized, ChordRhythm::Dotted, 4.0)) == noteStarts (renderChords (resized, ChordRhythm::Dotted)));
        for (const auto* q : { &waltz, &resized })
            for (double bar : { 2.0, 3.0, 3.5, 4.0, 5.0, 6.0 })
            {
                const auto notes = renderChords (*q, ChordRhythm::Dotted, bar);
                CHECK (notesFollowChords (notes, *q) && noOverlaps (notes));
                for (const auto& n : notes)
                    CHECK (n.length > 0.0 && n.length <= 1.4 + 1e-9);
            }
        // The first hit of each chord is accented.
        const auto accents = renderChords (resized, ChordRhythm::Dotted);
        for (const auto& n : accents)
            CHECK (n.velocity == ((n.start == 0.0 || n.start == 5.0) ? 100 : 86));
        for (auto r : bassRhythms)  CHECK (notesFollowChords (renderBass (uneven, r), uneven));
        CHECK (renderChords (Progression {}, ChordRhythm::Quarters).empty() && renderBass (Progression {}, BassRhythm::Eighths).empty());

        // Smooth voice leading: no voice has to jump far between neighbouring chords, sevenths included.
        const Key cMajor { 0, false, 1.0f };
        const auto loop = withSevenths (toProgression ({ { 0, Quality::Major }, { 9, Quality::Minor }, { 5, Quality::Major }, { 7, Quality::Major },
                                                         { 4, Quality::Minor }, { 2, Quality::Minor }, { 7, Quality::Major }, { 0, Quality::Major } }, 4.0), cMajor);
        const auto voiced = renderChords (loop, ChordRhythm::Sustain);
        double previousCentre = 0.0;
        for (size_t i = 0; i < loop.size(); ++i)
        {
            const auto pitches = pitchesAt (voiced, loop[i].start);
            CHECK (pitches.size() == 4);
            double centre = 0.0;
            for (int pitch : pitches) centre += pitch / 4.0;
            if (i > 0) CHECK (std::abs (centre - previousCentre) <= 4.0);
            CHECK (centre > 52.0 && centre < 70.0);
            previousCentre = centre;
        }

        // The fixed-grid forms are the same thing.
        const std::vector<Chord> grid { { 0, Quality::Major }, { 7, Quality::Major }, { 9, Quality::Minor }, { 5, Quality::Major } };
        for (auto r : chordRhythms)
        {
            const auto a = renderChords (grid, 4.0, r), b = renderChords (toProgression (grid, 4.0), r);
            bool same = a.size() == b.size();
            for (size_t i = 0; same && i < a.size(); ++i)
                same = a[i].pitch == b[i].pitch && a[i].start == b[i].start && a[i].length == b[i].length && a[i].velocity == b[i].velocity;
            CHECK (same);
        }
        // Triads are voiced exactly as before: C G Am F in close position around middle C.
        const auto triads = renderChords (grid, 4.0, ChordRhythm::Sustain);
        CHECK (pitchesAt (triads, 0.0) == (std::vector<int> { 55, 60, 64 }));
        CHECK (pitchesAt (triads, 4.0) == (std::vector<int> { 55, 59, 62 }));
        CHECK (pitchesAt (triads, 8.0) == (std::vector<int> { 52, 57, 60 }));
        CHECK (pitchesAt (triads, 12.0) == (std::vector<int> { 53, 57, 60 }));
    }

    // 17. Tempo estimate.
    {
        auto tempoOf = [] (const std::vector<float>& audio, double rate) { return estimateTempo (audio.data(), audio.size(), rate); };
        auto within = [] (double found, double expected) { return std::abs (found - expected) <= expected * 0.02; };

        for (double tempo : { 70.0, 75.0, 84.0, 90.0, 100.0, 112.5, 120.0, 128.0, 133.0, 140.0, 150.0, 160.0, 174.0, 180.0 })
        {
            const double found = tempoOf (clickTrack (44100.0, 16.0, tempo), 44100.0);
            std::printf ("tempo test: clicks at %.1f BPM came out as %.2f\n", tempo, found);
            CHECK (within (found, tempo));
        }
        for (double tempo : { 80.0, 92.0, 100.0, 110.0, 120.0, 126.0, 128.0, 135.0, 150.0 })
            for (double rate : { 44100.0, 48000.0 })
            {
                const double found = tempoOf (drumLoop (rate, 16.0, tempo), rate);
                std::printf ("tempo test: drum loop at %.1f BPM, %.0f Hz came out as %.2f\n", tempo, rate, found);
                CHECK (within (found, tempo));
            }
        CHECK (within (tempoOf (drumLoop (96000.0, 12.0, 105.0), 96000.0), 105.0));

        // A fast 3/4 loop is not read as one and a half of its beats (every other one between two real ones).
        for (double tempo : { 90.0, 120.0, 140.0, 150.0, 155.0, 160.0, 170.0, 180.0 })
        {
            const double found = tempoOf (waltzLoop (44100.0, 16.0, tempo), 44100.0);
            std::printf ("tempo test: 3/4 loop at %.1f BPM came out as %.2f\n", tempo, found);
            CHECK (std::abs (found - tempo) <= tempo * 0.01);
        }
        CHECK (within (tempoOf (drumLoop (22050.0, 12.0, 98.0), 22050.0), 98.0));

        // A pulse outside 70-180 comes back as its double or half, never as itself.
        const double outside[][2] = { { 60.0, 120.0 }, { 50.0, 100.0 }, { 40.0, 80.0 }, { 65.0, 130.0 },
                                      { 200.0, 100.0 }, { 240.0, 120.0 }, { 190.0, 95.0 } };
        for (const auto& pair : outside)
        {
            const double found = tempoOf (clickTrack (44100.0, 20.0, pair[0]), 44100.0);
            std::printf ("tempo test: clicks at %.1f BPM came out as %.2f\n", pair[0], found);
            CHECK (within (found, pair[1]));
        }
        // Whatever goes in, the answer is 0 (unknown) or inside the range.
        for (double tempo = 30.0; tempo <= 330.0; tempo += 25.0)
        {
            const double clicks = tempoOf (clickTrack (44100.0, 12.0, tempo), 44100.0);
            const double loop = tempoOf (drumLoop (44100.0, 12.0, tempo), 44100.0);
            CHECK (clicks == 0.0 || (clicks >= minTempo && clicks <= maxTempo)); // 30 BPM clicks have no pulse in range
            CHECK (loop >= minTempo && loop <= maxTempo);
            // ...and it is the pulse itself or a simple multiple of it.
            bool related = false;
            for (double ratio : { 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0, 1.5, 2.0, 3.0, 4.0 })
                related = related || std::abs (loop - tempo * ratio) <= tempo * ratio * 0.02;
            if (! related) std::printf ("tempo test: drum loop at %.1f BPM came out as %.2f\n", tempo, loop);
            CHECK (related);
        }

        // Whole tempos come out whole, and others are not forced onto one.
        CHECK (tempoOf (clickTrack (44100.0, 16.0, 120.0), 44100.0) == 120.0);
        CHECK (tempoOf (drumLoop (48000.0, 16.0, 128.0), 48000.0) == 128.0);
        CHECK (tempoOf (drumLoop (44100.0, 16.0, 95.0), 44100.0) == 95.0);
        CHECK (std::abs (tempoOf (clickTrack (44100.0, 16.0, 112.5), 44100.0) - 112.5) < 0.2);

        // A quiet loop is the same loop.
        auto quiet = drumLoop (44100.0, 12.0, 124.0);
        for (auto& v : quiet) v *= 0.001f;
        CHECK (within (tempoOf (quiet, 44100.0), 124.0));

        // Nothing to find.
        const std::vector<float> silence ((size_t) (44100.0 * 10.0));
        CHECK (tempoOf (silence, 44100.0) == 0.0);
        CHECK (tempoOf (clickTrack (44100.0, 1.5, 120.0), 44100.0) == 0.0);      // too short
        CHECK (estimateTempo (nullptr, 0, 44100.0) == 0.0);
        CHECK (estimateTempo (silence.data(), silence.size(), 0.0) == 0.0);
        const auto held = sine (44100.0, 10.0, 57, 0.4f);
        const double heldTempo = tempoOf (held, 44100.0);
        CHECK (heldTempo == 0.0);
        std::vector<float> oneNote ((size_t) (sampleRate * 10.0));
        addTone (oneNote, 1.0, 8.0, 57, 0.3f);
        CHECK (tempoOf (oneNote, sampleRate) == 0.0);
        const double noiseTempo = tempoOf (noise ((size_t) (44100.0 * 10.0), 0.3f), 44100.0);
        std::printf ("tempo test: held note %.2f, noise %.2f\n", heldTempo, noiseTempo);
        CHECK (noiseTempo == 0.0 || (noiseTempo >= minTempo && noiseTempo <= maxTempo));
    }

    std::printf (failures == 0 ? "All tests passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
