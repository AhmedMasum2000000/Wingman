#include "Backline.h"
#include "Random.h"
#include <algorithm>
#include <cmath>

namespace backline
{
namespace
{
const char* const noteNames[12]  = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
const char* const sharpNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char* const flatNames[12]  = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };

int wrapPitchClass (int pitchClass) { return ((pitchClass % 12) + 12) % 12; }

// How natural it sounds to move from one scale degree to another (rows: from, columns: to).
// Major: I ii iii IV V vi
const float majorMoves[6][6] = {
    { 0.1f, 0.5f, 0.3f, 0.9f, 0.9f, 0.8f },
    { 0.4f, 0.1f, 0.3f, 0.5f, 0.9f, 0.4f },
    { 0.3f, 0.4f, 0.1f, 0.8f, 0.3f, 0.8f },
    { 0.8f, 0.5f, 0.3f, 0.1f, 0.9f, 0.6f },
    { 0.9f, 0.3f, 0.3f, 0.6f, 0.1f, 0.9f },
    { 0.5f, 0.6f, 0.5f, 0.9f, 0.7f, 0.1f },
};
// Minor: i III iv v VI VII V
const float minorMoves[7][7] = {
    { 0.1f, 0.7f, 0.8f, 0.6f, 0.9f, 0.9f, 0.6f },
    { 0.5f, 0.1f, 0.6f, 0.4f, 0.7f, 0.8f, 0.3f },
    { 0.8f, 0.4f, 0.1f, 0.7f, 0.5f, 0.7f, 0.8f },
    { 0.8f, 0.4f, 0.5f, 0.1f, 0.8f, 0.4f, 0.1f },
    { 0.6f, 0.7f, 0.6f, 0.5f, 0.1f, 0.9f, 0.7f },
    { 0.9f, 0.8f, 0.4f, 0.4f, 0.6f, 0.1f, 0.3f },
    { 1.0f, 0.2f, 0.3f, 0.1f, 0.6f, 0.2f, 0.1f },
};

// Root, third (or suspension) and fifth; a 7th does not change how well a chord fits.
float fit (const Chroma& c, Chord chord)
{
    const auto pcs = chordPitchClasses (chord);
    return 1.1f * c[(size_t) pcs[0]] + 1.0f * c[(size_t) pcs[1]] + 0.9f * c[(size_t) pcs[2]];
}

float move (Key key, size_t from, size_t to)
{
    return key.minor ? minorMoves[from][to] : majorMoves[from][to];
}
}

std::string pitchClassName (int pitchClass) { return noteNames[wrapPitchClass (pitchClass)]; }

// Spelled the way the key is written: sharps in sharp keys, flats in flat keys.
std::string pitchClassName (int pitchClass, Key key)
{
    const int pc = wrapPitchClass (pitchClass);
    const int relativeMajor = wrapPitchClass (key.tonic + (key.minor ? 3 : 0));
    // Position on the circle of fifths: 1 to 6 are the sharp keys G D A E B F#, 7 to 11 the flat keys Db Ab Eb Bb F.
    const int fifths = (relativeMajor * 7) % 12;
    // The raised 7th of a minor key is the note under the tonic, sharpened: G# in A minor, C# in D minor.
    if (key.minor && pc == wrapPitchClass (key.tonic + 11))
        return sharpNames[pc];
    if (fifths == 0)
        return noteNames[pc]; // C major and A minor have no accidentals to follow
    if (fifths == 6 && key.minor)
        return pc == 11 ? "Cb" : flatNames[pc]; // written as Eb minor (six flats), not D# minor
    return fifths <= 6 ? sharpNames[pc] : flatNames[pc];
}

std::string qualitySuffix (Quality quality)
{
    switch (quality)
    {
        case Quality::Major:      return "";
        case Quality::Minor:      return "m";
        case Quality::Dominant7:  return "7";
        case Quality::Major7:     return "maj7";
        case Quality::Minor7:     return "m7";
        case Quality::Sus2:       return "sus2";
        case Quality::Sus4:       return "sus4";
        case Quality::Diminished: return "dim";
        case Quality::Add9:       return "add9";
        case Quality::MinorAdd9:  return "madd9";
        case Quality::Major9:     return "maj9";
        case Quality::Minor9:     return "m9";
        case Quality::Dominant9:  return "9";
    }
    return "";
}

std::string chordName (Chord chord)
{
    const int root = wrapPitchClass (chord.root);
    std::string name = (chord.sharpRoot ? sharpNames[root] : noteNames[root]) + qualitySuffix (chord.quality);
    if (chord.bassPitchClass() != wrapPitchClass (chord.root))
        name += "/" + pitchClassName (chord.bassPitchClass());
    return name;
}

std::string chordName (Chord chord, Key key)
{
    // A chord that leads up a semitone from outside the key is written with a sharp (F#dim
    // before Gm in F major), whatever the key's own accidentals.
    const int root = wrapPitchClass (chord.root);
    std::string name = (chord.sharpRoot && ! isInScale (root, key) ? std::string (sharpNames[root]) : pitchClassName (root, key))
                     + qualitySuffix (chord.quality);
    if (chord.bassPitchClass() != wrapPitchClass (chord.root))
        name += "/" + pitchClassName (chord.bassPitchClass(), key);
    return name;
}

std::vector<int> chordIntervals (Quality quality)
{
    switch (quality)
    {
        case Quality::Major:      return { 0, 4, 7 };
        case Quality::Minor:      return { 0, 3, 7 };
        case Quality::Dominant7:  return { 0, 4, 7, 10 };
        case Quality::Major7:     return { 0, 4, 7, 11 };
        case Quality::Minor7:     return { 0, 3, 7, 10 };
        case Quality::Sus2:       return { 0, 2, 7 };
        case Quality::Sus4:       return { 0, 5, 7 };
        case Quality::Diminished: return { 0, 3, 6 };
        case Quality::Add9:       return { 0, 4, 7, 14 };
        case Quality::MinorAdd9:  return { 0, 3, 7, 14 };
        case Quality::Major9:     return { 0, 4, 7, 11, 14 };
        case Quality::Minor9:     return { 0, 3, 7, 10, 14 };
        case Quality::Dominant9:  return { 0, 4, 7, 10, 14 };
    }
    return { 0, 4, 7 };
}

std::vector<int> chordPitchClasses (Chord chord)
{
    auto pcs = chordIntervals (chord.quality);
    for (auto& pc : pcs)
        pc = wrapPitchClass (chord.root + pc);
    return pcs;
}

bool hasSeventh (Quality quality)
{
    return quality == Quality::Dominant7 || quality == Quality::Major7 || quality == Quality::Minor7
        || quality == Quality::Major9 || quality == Quality::Minor9 || quality == Quality::Dominant9;
}

bool hasMinorThird (Quality quality)
{
    return quality == Quality::Minor || quality == Quality::Minor7 || quality == Quality::Diminished
        || quality == Quality::MinorAdd9 || quality == Quality::Minor9;
}

bool isInScale (int pitchClass, Key key)
{
    static const bool major[12] = { true, false, true, false, true, true, false, true, false, true, false, true };
    static const bool minor[12] = { true, false, true, true, false, true, false, true, true, false, true, false };
    const int degree = wrapPitchClass (pitchClass - key.tonic);
    return key.minor ? minor[degree] : major[degree];
}

std::string keyName (Key key)
{
    return pitchClassName (key.tonic, key) + (key.minor ? " minor" : " major");
}

std::vector<Chord> chordsInKey (Key key)
{
    const int t = wrapPitchClass (key.tonic);
    auto ch = [t] (int interval, Quality q) { return Chord { (t + interval) % 12, q }; };
    if (key.minor)
        return { ch (0, Quality::Minor), ch (3, Quality::Major), ch (5, Quality::Minor), ch (7, Quality::Minor),
                 ch (8, Quality::Major), ch (10, Quality::Major), ch (7, Quality::Major) };
    return { ch (0, Quality::Major), ch (2, Quality::Minor), ch (4, Quality::Minor),
             ch (5, Quality::Major), ch (7, Quality::Major), ch (9, Quality::Minor) };
}

std::vector<Chord> rankChords (const Chroma& segment, Key key)
{
    auto chords = chordsInKey (key);
    std::stable_sort (chords.begin(), chords.end(),
                      [&] (Chord a, Chord b) { return fit (segment, a) > fit (segment, b); });
    return chords;
}

std::vector<std::vector<Chord>> suggestProgressions (const std::vector<Chroma>& segments, Key key, int count)
{
    std::vector<std::vector<Chord>> ideas;
    const auto chords = chordsInKey (key);
    const size_t numSegments = segments.size(), numChords = chords.size();
    if (numSegments == 0)
        return ideas;

    // There cannot be more different ideas than there are chord sequences.
    size_t wanted = (size_t) std::max (0, count);
    std::uint64_t sequences = 1;
    for (size_t s = 0; s < numSegments && sequences < wanted; ++s)
        sequences *= numChords;
    if (sequences < wanted)
        wanted = (size_t) sequences;

    const float fitWeight = 2.0f, moveWeight = 0.5f, tonicBonus = 0.3f, reusePenalty = 0.6f;
    std::vector<std::vector<float>> penalty (numSegments, std::vector<float> (numChords, 0.0f));

    // A path that repeats an earlier idea is dropped. Its penalties have already been raised,
    // so the next attempt is pushed further away from it.
    for (size_t attempt = 0; ideas.size() < wanted && attempt < wanted * 16; ++attempt)
    {
        // Viterbi: best-scoring path of chords through the segments.
        std::vector<std::vector<float>> score (numSegments, std::vector<float> (numChords));
        std::vector<std::vector<size_t>> back (numSegments, std::vector<size_t> (numChords, 0));
        for (size_t c = 0; c < numChords; ++c)
            score[0][c] = fitWeight * fit (segments[0], chords[c]) - penalty[0][c] + (c == 0 ? tonicBonus : 0.0f);

        for (size_t s = 1; s < numSegments; ++s)
            for (size_t c = 0; c < numChords; ++c)
            {
                float best = -1.0e9f;
                size_t bestPrev = 0;
                for (size_t p = 0; p < numChords; ++p)
                {
                    const float v = score[s - 1][p] + moveWeight * move (key, p, c);
                    if (v > best) { best = v; bestPrev = p; }
                }
                score[s][c] = best + fitWeight * fit (segments[s], chords[c]) - penalty[s][c];
                back[s][c] = bestPrev;
            }

        size_t current = (size_t) (std::max_element (score.back().begin(), score.back().end()) - score.back().begin());
        std::vector<Chord> path (numSegments);
        for (size_t s = numSegments; s-- > 0;)
        {
            path[s] = chords[current];
            penalty[s][current] += reusePenalty; // push later ideas towards different chords
            current = back[s][current];
        }
        if (std::find (ideas.begin(), ideas.end(), path) == ideas.end())
            ideas.push_back (path);
    }

    // Safety net for input the penalties cannot separate (chroma on a far larger scale than the
    // penalty): step through combinations of each segment's best-fitting chords, last segment first.
    if (ideas.size() < wanted)
    {
        std::vector<std::vector<Chord>> ranked;
        for (const auto& segment : segments)
            ranked.push_back (rankChords (segment, key));

        std::vector<size_t> choice (numSegments, 0);
        while (ideas.size() < wanted)
        {
            std::vector<Chord> path (numSegments);
            for (size_t s = 0; s < numSegments; ++s)
                path[s] = ranked[s][choice[s]];
            if (std::find (ideas.begin(), ideas.end(), path) == ideas.end())
                ideas.push_back (path);

            for (size_t s = numSegments; s-- > 0;)
            {
                if (++choice[s] < numChords)
                    break;
                choice[s] = 0;
            }
        }
    }
    return ideas;
}

std::vector<std::vector<Chord>> suggestProgressions (const std::vector<Chroma>& segments, Key key, int count, std::uint32_t seed)
{
    if (seed == 0)
        return suggestProgressions (segments, key, count);
    auto heard = segments;
    for (size_t s = 0; s < heard.size(); ++s)
    {
        Random random (mixSeed (seed, s));
        for (auto& v : heard[s])
            v *= (float) (0.55 + 0.9 * random.uniform());
    }
    return suggestProgressions (heard, key, count);
}

// ---- Progressions ---------------------------------------------------------------------------

namespace
{
const double tiny = 1.0e-9;

// Start of every chord plus the end of the last one.
std::vector<double> boundariesOf (const Progression& progression)
{
    std::vector<double> bounds;
    for (const auto& e : progression)
        bounds.push_back (e.start);
    if (! progression.empty())
        bounds.push_back (progression.back().start + progression.back().length);
    return bounds;
}

// The events placed on new boundaries (event i from bounds[i] to bounds[i + 1]); each keeps its
// chord and flags.
Progression withBoundaries (Progression events, const std::vector<double>& bounds)
{
    for (size_t i = 0; i < events.size(); ++i)
    {
        events[i].start = bounds[i];
        events[i].length = bounds[i + 1] - bounds[i];
    }
    return events;
}

// In a minor key the raised 7th (the third of the major V) belongs as well.
bool fitsKey (int pitchClass, Key key)
{
    return isInScale (pitchClass, key) || (key.minor && wrapPitchClass (pitchClass - key.tonic) == 11);
}

bool sameChordType (Chord a, Chord b)
{
    return wrapPitchClass (a.root) == wrapPitchClass (b.root) && a.quality == b.quality;
}
}

Progression toProgression (const std::vector<Chord>& chords, double beatsPerChord)
{
    Progression result;
    for (size_t i = 0; i < chords.size(); ++i)
        result.push_back ({ chords[i], (double) i * beatsPerChord, beatsPerChord });
    return result;
}

std::vector<Chord> chordsOf (const Progression& progression)
{
    std::vector<Chord> chords;
    for (const auto& e : progression)
        chords.push_back (e.chord);
    return chords;
}

double progressionLength (const Progression& progression)
{
    return progression.empty() ? 0.0 : progression.back().start + progression.back().length;
}

bool isValidProgression (const Progression& progression)
{
    if (progression.empty())
        return false;
    double expected = 0.0;
    for (const auto& e : progression)
    {
        if (! (e.length > tiny) || std::abs (e.start - expected) > tiny)
            return false;
        expected = e.start + e.length;
    }
    return true;
}

int chordIndexAt (const Progression& progression, double beat)
{
    for (size_t i = 0; i < progression.size(); ++i)
        if (beat >= progression[i].start && beat < progression[i].start + progression[i].length)
            return (int) i;
    return -1;
}

Progression setChord (const Progression& progression, size_t index, Chord chord)
{
    Progression result = progression;
    if (index < result.size())
    {
        result[index].chord = chord;
        result[index].coloured = false;
    }
    return result;
}

Progression splitChord (const Progression& progression, size_t index, double gridBeats)
{
    if (index >= progression.size())
        return progression;

    const double length = progression[index].length;
    double first = length * 0.5;
    if (gridBeats > tiny)
    {
        if (length < 2.0 * gridBeats - tiny)
            return progression;
        first = std::round (first / gridBeats) * gridBeats;
        first = std::min (std::max (first, gridBeats), length - gridBeats);
    }
    if (first < tiny || length - first < tiny)
        return progression;

    auto events = progression;
    auto bounds = boundariesOf (progression);
    events.insert (events.begin() + (std::ptrdiff_t) index, progression[index]);
    bounds.insert (bounds.begin() + (std::ptrdiff_t) index + 1, progression[index].start + first);
    return withBoundaries (std::move (events), bounds);
}

Progression moveBoundary (const Progression& progression, size_t boundary, double beat, double gridBeats)
{
    if (boundary + 1 >= progression.size())
        return progression;

    auto bounds = boundariesOf (progression);
    const double step = std::max (gridBeats, 0.0);
    const double lowest = bounds[boundary] + step, highest = bounds[boundary + 2] - step;
    if (lowest > highest + tiny)
        return progression; // no room for both chords to keep a grid step

    double target = beat;
    if (step > tiny)
    {
        target = std::round (beat / step) * step;
        // Outside the allowed range: the nearest grid line inside it, or its edge if no line is.
        if (target < lowest - tiny)
        {
            target = std::ceil (lowest / step - tiny) * step;
            if (target > highest + tiny) target = lowest;
        }
        else if (target > highest + tiny)
        {
            target = std::floor (highest / step + tiny) * step;
            if (target < lowest - tiny) target = highest;
        }
    }
    target = std::min (std::max (target, lowest), highest);
    if (target - bounds[boundary] < tiny || bounds[boundary + 2] - target < tiny)
        return progression;

    bounds[boundary + 1] = target;
    return withBoundaries (progression, bounds);
}

Progression deleteChord (const Progression& progression, size_t index)
{
    if (progression.size() < 2 || index >= progression.size())
        return progression;

    auto events = progression;
    auto bounds = boundariesOf (progression);
    events.erase (events.begin() + (std::ptrdiff_t) index);
    // Dropping a chord's start hands its time to the chord before it. The first chord's start
    // stays (the loop begins at 0) and the next start goes instead, which hands the time forward.
    bounds.erase (bounds.begin() + (std::ptrdiff_t) (index == 0 ? 1 : index));
    return withBoundaries (std::move (events), bounds);
}

Chord passingChord (Chord from, Chord to, Key key)
{
    const int fromRoot = wrapPitchClass (from.root), toRoot = wrapPitchClass (to.root);
    const int up = wrapPitchClass (toRoot - fromRoot);
    const auto diatonic = chordsInKey (key);
    auto usable = [&] (Chord c) { return ! sameChordType (c, from) && ! sameChordType (c, to); };

    // A tone up: the diminished chord on the semitone between the two roots, which leads up
    // into the next chord (spelled with a sharp: C#dim, F#dim).
    if (up == 2)
        return { (fromRoot + 1) % 12, Quality::Diminished, -1, true };

    // A third up: the key's chord on the scale note between the roots (C, Dm, Em).
    if (up == 3 || up == 4)
        for (int step = 1; step < up; ++step)
            for (auto c : diatonic)
                if (c.root == (fromRoot + step) % 12 && usable (c))
                    return c;

    // A third down: walk the bass down through the scale note between the roots, under the
    // key's chord that has that note as its third (C, G/B, Am).
    if (up == 8 || up == 9)
        for (int step = 1; step < 12 - up; ++step)
        {
            const int bass = wrapPitchClass (fromRoot - step);
            if (isInScale (bass, key))
                for (auto c : diatonic)
                    if (chordPitchClasses (c)[1] == bass && usable (c))
                        return { c.root, c.quality, bass };
        }

    // Anything else is approached by its own dominant 7th, or by the diminished chord on its
    // leading note when the dominant is what we are coming from.
    const Chord dominant { (toRoot + 7) % 12, Quality::Dominant7 };
    if (! sameChordType (dominant, from))
        return dominant;
    return { (toRoot + 11) % 12, Quality::Diminished, -1, true };
}

Progression insertPassingChord (const Progression& progression, size_t index, Key key)
{
    if (index >= progression.size() || progression[index].length < 0.25 - tiny)
        return progression;

    const auto& event = progression[index];
    const double taken = event.length >= 2.0 - tiny ? 1.0 : event.length * 0.5;
    const Chord next = progression[(index + 1) % progression.size()].chord;

    ChordEvent passing;
    passing.chord = passingChord (event.chord, next, key);
    auto events = progression;
    auto bounds = boundariesOf (progression);
    bounds.insert (bounds.begin() + (std::ptrdiff_t) index + 1, event.start + event.length - taken);
    events.insert (events.begin() + (std::ptrdiff_t) index + 1, passing);
    return withBoundaries (std::move (events), bounds);
}

std::vector<Chord> chordVariations (Chord chord, Key key)
{
    const int root = wrapPitchClass (chord.root);
    const bool rootInKey = fitsKey (root, key);
    std::vector<Chord> variations;
    auto add = [&] (Chord c)
    {
        if (c != chord && std::find (variations.begin(), variations.end(), c) == variations.end())
            variations.push_back (c);
    };

    for (int q = 0; q < numBasicQualities; ++q)
    {
        const Chord candidate { root, (Quality) q, -1, chord.sharpRoot };
        bool fits = true;
        for (int pc : chordPitchClasses (candidate))
            fits = fits && fitsKey (pc, key);
        if (fits || ! rootInKey || candidate.quality == chord.quality)
            add (candidate);
    }

    const auto pcs = chordPitchClasses (chord);
    add ({ root, chord.quality, pcs[1], chord.sharpRoot });
    add ({ root, chord.quality, pcs[2], chord.sharpRoot });
    return variations;
}

Chord seventhChord (Chord chord, Key key)
{
    if (chord.quality != Quality::Major && chord.quality != Quality::Minor)
        return chord;
    if (! isInScale (chord.root, key))
        return chord;

    const bool major = chord.quality == Quality::Major;
    Chord result = chord;
    if (isInScale (chord.root + 10, key))
        result.quality = major ? Quality::Dominant7 : Quality::Minor7;
    else if (major && isInScale (chord.root + 11, key))
        result.quality = Quality::Major7;
    return result;
}

Progression withSevenths (const Progression& progression, Key key)
{
    Progression result = progression;
    for (auto& e : result)
    {
        const Chord coloured = seventhChord (e.chord, key);
        if (coloured != e.chord)
        {
            e.chord = coloured;
            e.coloured = true;
        }
    }
    return result;
}

Chord triadChord (Chord chord)
{
    Chord result = chord;
    if (chord.quality == Quality::Dominant7 || chord.quality == Quality::Major7 || chord.quality == Quality::Add9
        || chord.quality == Quality::Major9 || chord.quality == Quality::Dominant9)
        result.quality = Quality::Major;
    else if (chord.quality == Quality::Minor7 || chord.quality == Quality::MinorAdd9 || chord.quality == Quality::Minor9)
        result.quality = Quality::Minor;
    // A 7th in the bass has nothing to stand on once the 7th is gone.
    if (result.quality != chord.quality && chord.bass >= 0)
    {
        const auto pcs = chordPitchClasses (result);
        if (std::find (pcs.begin(), pcs.end(), chord.bassPitchClass()) == pcs.end())
            result.bass = -1;
    }
    return result;
}

Progression withoutSevenths (const Progression& progression)
{
    Progression result = progression;
    for (auto& e : result)
        if (e.coloured)
        {
            e.chord = triadChord (e.chord);
            e.coloured = false;
        }
    return result;
}
}
