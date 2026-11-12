#include "Backline.h"
#include <algorithm>
#include <cmath>

namespace backline
{
namespace
{
int pitchClassOf (int pitch) { return ((pitch % 12) + 12) % 12; }

// Voice a chord close to the previous one so the part moves smoothly: every chord tone goes
// into the octave around `centre`. The bass note of a slash chord sits below all of them.
std::vector<int> voice (Chord chord, double centre)
{
    const int lowest = (int) std::floor (centre) - 6;
    const int bass = chord.bassPitchClass();
    const bool slash = bass != pitchClassOf (chord.root);

    std::vector<int> pitches;
    for (int pc : chordPitchClasses (chord))
    {
        if (slash && pc == bass)
            continue; // it goes underneath instead
        int pitch = lowest;
        while (pitchClassOf (pitch) != pc) ++pitch;
        pitches.push_back (pitch);
    }
    std::sort (pitches.begin(), pitches.end());

    if (slash)
    {
        int pitch = pitches.front() - 1;
        while (pitchClassOf (pitch) != bass) --pitch;
        pitches.insert (pitches.begin(), pitch);
    }
    return pitches;
}

struct Hits
{
    std::vector<double> starts, lengths;

    void add (double start, double length) { starts.push_back (start); lengths.push_back (length); }

    void addEvery (double segment, double step, double offset, double length)
    {
        for (double t = offset; t < segment - 1.0e-9; t += step)
            add (t, std::min (length, segment - t));
    }

    // A chord too short for its rhythm (an offbeat pattern on a half-beat chord) is still heard.
    void ensureOne (double segment)
    {
        if (starts.empty())
            add (0.0, segment);
    }
};
}

std::vector<Note> renderChords (const Progression& progression, ChordRhythm rhythm, double beatsPerBar)
{
    const double bar = beatsPerBar > 1.0e-9 ? beatsPerBar : 4.0;
    std::vector<Note> notes;
    double centre = 60.0;
    for (const auto& event : progression)
    {
        const auto pitches = voice (event.chord, centre);
        double sum = 0.0;
        for (int p : pitches) sum += (double) p;
        centre = std::min (std::max (sum / (double) pitches.size(), 55.0), 67.0);

        const double span = event.length;
        Hits hits;
        switch (rhythm)
        {
            case ChordRhythm::Sustain:  hits.add (0.0, span); break;
            case ChordRhythm::Quarters: hits.addEvery (span, 1.0, 0.0, 0.9); break;
            case ChordRhythm::Offbeat:  hits.addEvery (span, 1.0, 0.5, 0.4); break;
            case ChordRhythm::Dotted:
            {
                // A dotted quarter figure that starts again on every bar line of the loop. Each
                // hit lasts until shortly before the next one (or the bar line).
                const double end = event.start + span;
                std::vector<double> starts, lengths;
                for (double barStart = std::floor (event.start / bar + 1.0e-9) * bar; barStart < end - 1.0e-9; barStart += bar)
                    for (double t = 0.0; t < bar - 1.0e-9; t += 1.5)
                    {
                        const double hit = barStart + t;
                        if (hit < event.start - 1.0e-9 || hit >= end - 1.0e-9)
                            continue;
                        const double next = std::min (hit + 1.5, barStart + bar);
                        starts.push_back (hit - event.start);
                        lengths.push_back (std::min (std::min (1.4, std::max (next - hit - 0.1, 0.5 * (next - hit))), end - hit));
                    }
                // A chord that starts between the hits is struck on its start as well.
                if (starts.empty() || starts.front() > 1.0e-9)
                    hits.add (0.0, std::min (1.4, starts.empty() ? span : starts.front()));
                for (size_t h = 0; h < starts.size(); ++h)
                    hits.add (std::max (0.0, starts[h]), lengths[h]);
                break;
            }
        }
        hits.ensureOne (span);

        for (size_t h = 0; h < hits.starts.size(); ++h)
            for (int p : pitches)
                notes.push_back ({ p, event.start + hits.starts[h], hits.lengths[h], h == 0 ? 100 : 86 });
    }
    return notes;
}

std::vector<Note> renderBass (const Progression& progression, BassRhythm rhythm)
{
    std::vector<Note> notes;
    for (const auto& event : progression)
    {
        const Chord chord = event.chord;
        const int low = 36 + chord.bassPitchClass(); // C2..B2

        // The note the root-and-fifth pattern alternates with: the chord's fifth, or for a
        // slash chord the root above the bass note.
        int other = low + chordIntervals (chord.quality)[2];
        if (chord.bassPitchClass() != pitchClassOf (chord.root))
        {
            other = low + 1;
            while (pitchClassOf (other) != pitchClassOf (chord.root)) ++other;
        }

        const double span = event.length;
        Hits hits;
        switch (rhythm)
        {
            case BassRhythm::Sustain:   hits.add (0.0, span); break;
            case BassRhythm::Eighths:
            case BassRhythm::Octaves:   hits.addEvery (span, 0.5, 0.0, 0.45); break;
            case BassRhythm::RootFifth: hits.addEvery (span, 1.0, 0.0, 0.9); break;
            case BassRhythm::Offbeat:   hits.addEvery (span, 1.0, 0.5, 0.45); break;
        }
        hits.ensureOne (span);

        for (size_t h = 0; h < hits.starts.size(); ++h)
        {
            int pitch = low;
            if (rhythm == BassRhythm::Octaves && h % 2 == 1) pitch += 12;
            if (rhythm == BassRhythm::RootFifth && h % 2 == 1) pitch = other;
            notes.push_back ({ pitch, event.start + hits.starts[h], hits.lengths[h], h == 0 ? 110 : 96 });
        }
    }
    return notes;
}

std::vector<Note> renderChords (const std::vector<Chord>& progression, double beatsPerChord, ChordRhythm rhythm)
{
    return renderChords (toProgression (progression, beatsPerChord), rhythm);
}

std::vector<Note> renderBass (const std::vector<Chord>& progression, double beatsPerChord, BassRhythm rhythm)
{
    return renderBass (toProgression (progression, beatsPerChord), rhythm);
}
}
