#include "Backline.h"
#include <algorithm>
#include <cmath>

namespace backline
{
namespace
{
// Voice a chord close to the previous one so the part moves smoothly.
std::array<int, 3> voice (Chord chord, double centre)
{
    const int intervals[3] = { 0, chord.quality == Quality::Major ? 4 : 3, 7 };
    std::array<int, 3> pitches {};
    for (size_t i = 0; i < 3; ++i)
    {
        const int pc = (chord.root + intervals[i]) % 12;
        int pitch = (int) std::floor (centre) - 6;
        while (pitch % 12 != pc) ++pitch;
        pitches[i] = pitch;
    }
    std::sort (pitches.begin(), pitches.end());
    return pitches;
}

void addHits (std::vector<double>& starts, std::vector<double>& lengths, double segment,
              double step, double offset, double length)
{
    for (double t = offset; t < segment - 1.0e-9; t += step)
    {
        starts.push_back (t);
        lengths.push_back (std::min (length, segment - t));
    }
}
}

std::vector<Note> renderChords (const std::vector<Chord>& progression, double beatsPerChord, ChordRhythm rhythm)
{
    std::vector<Note> notes;
    double centre = 60.0;
    for (size_t i = 0; i < progression.size(); ++i)
    {
        const auto pitches = voice (progression[i], centre);
        centre = std::clamp ((pitches[0] + pitches[1] + pitches[2]) / 3.0, 55.0, 67.0);

        std::vector<double> starts, lengths;
        switch (rhythm)
        {
            case ChordRhythm::Sustain:  starts.push_back (0.0); lengths.push_back (beatsPerChord); break;
            case ChordRhythm::Quarters: addHits (starts, lengths, beatsPerChord, 1.0, 0.0, 0.9); break;
            case ChordRhythm::Offbeat:  addHits (starts, lengths, beatsPerChord, 1.0, 0.5, 0.4); break;
            case ChordRhythm::Dotted:
                for (double bar = 0.0; bar < beatsPerChord - 1.0e-9; bar += 4.0)
                    for (double t : { 0.0, 1.5, 3.0 })
                        if (bar + t < beatsPerChord - 1.0e-9)
                        {
                            starts.push_back (bar + t);
                            lengths.push_back (std::min (t < 3.0 ? 1.4 : 0.9, beatsPerChord - bar - t));
                        }
                break;
        }
        const double base = (double) i * beatsPerChord;
        for (size_t h = 0; h < starts.size(); ++h)
            for (int p : pitches)
                notes.push_back ({ p, base + starts[h], lengths[h], h == 0 ? 100 : 86 });
    }
    return notes;
}

std::vector<Note> renderBass (const std::vector<Chord>& progression, double beatsPerChord, BassRhythm rhythm)
{
    std::vector<Note> notes;
    for (size_t i = 0; i < progression.size(); ++i)
    {
        const int root = 36 + progression[i].root; // C2..B2
        const double base = (double) i * beatsPerChord;
        std::vector<double> starts, lengths;
        switch (rhythm)
        {
            case BassRhythm::Sustain:   starts.push_back (0.0); lengths.push_back (beatsPerChord); break;
            case BassRhythm::Eighths:
            case BassRhythm::Octaves:   addHits (starts, lengths, beatsPerChord, 0.5, 0.0, 0.45); break;
            case BassRhythm::RootFifth: addHits (starts, lengths, beatsPerChord, 1.0, 0.0, 0.9); break;
            case BassRhythm::Offbeat:   addHits (starts, lengths, beatsPerChord, 1.0, 0.5, 0.45); break;
        }
        for (size_t h = 0; h < starts.size(); ++h)
        {
            int pitch = root;
            if (rhythm == BassRhythm::Octaves && h % 2 == 1) pitch += 12;
            if (rhythm == BassRhythm::RootFifth && h % 2 == 1) pitch += 7;
            notes.push_back ({ pitch, base + starts[h], lengths[h], h == 0 ? 110 : 96 });
        }
    }
    return notes;
}
}
