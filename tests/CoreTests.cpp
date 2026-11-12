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

    std::printf (failures == 0 ? "All tests passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
