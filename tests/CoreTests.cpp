#include "Backline.h"
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

    std::printf (failures == 0 ? "All tests passed\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
