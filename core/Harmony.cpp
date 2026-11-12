#include "Backline.h"
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

float fit (const Chroma& c, Chord chord)
{
    const int third = chord.quality == Quality::Major ? 4 : 3;
    return 1.1f * c[(size_t) chord.root]
         + 1.0f * c[(size_t) ((chord.root + third) % 12)]
         + 0.9f * c[(size_t) ((chord.root + 7) % 12)];
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
    if (fifths == 0)
        return noteNames[pc]; // C major and A minor have no accidentals to follow
    if (fifths == 6 && key.minor)
        return pc == 11 ? "Cb" : flatNames[pc]; // written as Eb minor (six flats), not D# minor
    return fifths <= 6 ? sharpNames[pc] : flatNames[pc];
}

std::string chordName (Chord chord)
{
    return pitchClassName (chord.root) + (chord.quality == Quality::Minor ? "m" : "");
}

std::string chordName (Chord chord, Key key)
{
    return pitchClassName (chord.root, key) + (chord.quality == Quality::Minor ? "m" : "");
}

std::string keyName (Key key)
{
    return pitchClassName (key.tonic, key) + (key.minor ? " minor" : " major");
}

std::vector<Chord> chordsInKey (Key key)
{
    const int t = key.tonic;
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
}
