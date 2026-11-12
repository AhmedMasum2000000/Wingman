#include "Backline.h"
#include <algorithm>
#include <cmath>

namespace backline
{
namespace
{
const char* const noteNames[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

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

std::string pitchClassName (int pitchClass) { return noteNames[((pitchClass % 12) + 12) % 12]; }

std::string chordName (Chord chord)
{
    return pitchClassName (chord.root) + (chord.quality == Quality::Minor ? "m" : "");
}

std::string keyName (Key key)
{
    return pitchClassName (key.tonic) + (key.minor ? " minor" : " major");
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

    const float fitWeight = 2.0f, moveWeight = 0.5f, tonicBonus = 0.3f, reusePenalty = 0.6f;
    std::vector<std::vector<float>> penalty (numSegments, std::vector<float> (numChords, 0.0f));

    for (int idea = 0; idea < count; ++idea)
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
        ideas.push_back (path);
    }
    return ideas;
}
}
