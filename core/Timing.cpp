// Detailed ("Auto") chord timing: beat-synchronous chroma, chord-change detection by a Viterbi
// pass over chord candidates, and timed ideas built on the detected timing.
#include "Backline.h"
#include "Fft.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib> // TUNE

namespace backline
{
namespace
{
const double pi = 3.14159265358979323846;
const double tiny = 1.0e-9;

int wrap (int pc) { return ((pc % 12) + 12) % 12; }

bool fitsKeyScale (int pitchClass, Key key)
{
    return isInScale (pitchClass, key) || (key.minor && wrap (pitchClass - key.tonic) == 11);
}

// A spectral peak of one analysis step: its pitch as a (fractional) MIDI note number, its
// frequency and its compressed level.
struct Peak
{
    double midi, hz;
    float level;
};

// The reference pitch of a recording, as an offset from A = 440 Hz in semitones (-0.5..0.5): the
// level-weighted circular mean of how far every peak sits from the nearest equal-tempered note.
// 0 when the peaks do not agree on one (too few, or spread all round).
double estimateTuning (const std::vector<std::vector<Peak>>& peaks)
{
    double c = 0.0, s = 0.0, total = 0.0;
    for (const auto& step : peaks)
        for (const auto& p : step)
        {
            if (p.hz < 60.0)
                continue; // low peaks are placed too coarsely to say anything about cents
            const double angle = 2.0 * pi * (p.midi - std::round (p.midi));
            c += (double) p.level * std::cos (angle);
            s += (double) p.level * std::sin (angle);
            total += (double) p.level;
        }
    if (! (total > 0.0) || std::sqrt (c * c + s * s) < 0.25 * total)
        return 0.0;
    return std::atan2 (s, c) / (2.0 * pi);
}

// The chords the detector chooses between: every root in each basic quality.
struct Candidate
{
    Chord chord;                    // root position
    Chroma shape {};                // unit-length template
    float prior = 0.0f;
    std::vector<int> bassChoices;   // pitch classes the bass may hold: root, third, fifth (and 7th)
};

std::vector<Candidate> candidatesFor (Key key)
{
    std::vector<Candidate> result;
    for (int root = 0; root < 12; ++root)
        for (int q = 0; q < numBasicQualities; ++q)
        {
            Candidate c;
            c.chord = { root, (Quality) q };
            const auto pcs = chordPitchClasses (c.chord);
            // Each note with a little of its third harmonic (the fifth above), as real sounds have.
            for (int pc : pcs)
            {
                c.shape[(size_t) pc] += 1.0f;
                c.shape[(size_t) wrap (pc + 7)] += 0.2f;
            }
            float norm = 0.0f;
            for (float v : c.shape) norm += v * v;
            norm = std::sqrt (norm);
            for (float& v : c.shape) v /= norm;

            const Quality quality = (Quality) q;
            float prior = 0.0f;
            if (hasSeventh (quality)) prior -= 0.025f;
            // Csus2 holds the notes of Gsus4: the sus4 name wins a tie.
            if (quality == Quality::Sus4) prior -= 0.045f;
            if (quality == Quality::Sus2) prior -= 0.05f;
            if (quality == Quality::Diminished) prior -= 0.05f;
            for (int pc : pcs)
                if (! fitsKeyScale (pc, key))
                    prior -= 0.035f;
            c.prior = prior;
            for (size_t i = 0; i < pcs.size() && i < 4; ++i)
                c.bassChoices.push_back (pcs[i]);
            result.push_back (c);
        }
    return result;
}

float cosine (const Chroma& a, const Chroma& unitShape)
{
    float dot = 0.0f, norm = 0.0f;
    for (size_t i = 0; i < 12; ++i)
    {
        dot += a[i] * unitShape[i];
        norm += a[i] * a[i];
    }
    return norm > 1.0e-12f ? dot / std::sqrt (norm) : 0.0f;
}

// Cosine similarity of two chroma vectors (0 when either is silent).
float cosineOf (const Chroma& a, const Chroma& b)
{
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < 12; ++i)
    {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    return na > 1.0e-12f && nb > 1.0e-12f ? dot / std::sqrt (na * nb) : 0.0f;
}

// How much a chord change costs at a step that starts on `beat`: least on a bar line, more on
// the half bar, more again on other beats and most between beats.
double positionWeight (double beat, double beatsPerBar)
{
    auto on = [beat] (double grid) { const double r = beat / grid; return std::abs (r - std::round (r)) < 1.0e-6; };
    if (on (beatsPerBar)) return 1.0;
    if (std::abs (std::fmod (beatsPerBar, 2.0)) < 1.0e-9 && on (beatsPerBar * 0.5)) return 1.6;
    if (on (1.0)) return 2.4;
    return 3.2;
}

// The cost of a chord change at the most detailed setting, and how much it grows towards Simple.
// At Simple a bar of a different chord is still kept, a passing chord of a beat or two is not.
double changeCost (double detail)
{
    const double simple = 1.0 - std::min (std::max (detail, 0.0), 1.0);
    return 0.012 + 0.45 * simple * simple;
}

// Detail is followed in steps of a twentieth (finer differences cannot be heard in the result).
const int detailLevels = 20;

// What an inversion costs against root position, per beat: with the third in the bass, and with
// any other note (fifth, seventh, or the second or fourth of a suspended chord).
static float tuneParam (const char* n, float d) { const char* v = std::getenv (n); return v ? (float) std::atof (v) : d; } // TUNE
const float thirdInBass = tuneParam ("B_THIRD", 0.03f), fifthInBass = tuneParam ("B_FIFTH", 0.07f), bassWeight = tuneParam ("B_WEIGHT", 0.15f); // TUNE
}

double analysisStepBeats (double bpm)
{
    return bpm > 0.0 && bpm < 90.0 ? 0.5 : 1.0;
}

BeatChroma analyseBeats (const float* mono, size_t numSamples, double sampleRate, double bpm, double loopBeats)
{
    BeatChroma result;
    result.stepBeats = analysisStepBeats (bpm);
    if (mono == nullptr || sampleRate <= 0.0 || bpm <= 0.0)
        return result;

    const double secondsPerBeat = 60.0 / bpm;
    const double stepSeconds = result.stepBeats * secondsPerBeat;
    const double audioBeats = (double) numSamples / sampleRate / secondsPerBeat;
    result.lengthBeats = loopBeats > tiny ? loopBeats : std::floor (audioBeats / result.stepBeats + 1.0e-6) * result.stepBeats;
    const size_t numSteps = (size_t) std::ceil (result.lengthBeats / result.stepBeats - 1.0e-6);
    result.chroma.assign (numSteps, Chroma {});
    result.bass.assign (numSteps, Chroma {});
    result.energy.assign (numSteps, 0.0f);
    if (numSteps == 0)
        return result;

    // A window over the middle of each step (the edges are left out, where the chord before or
    // after may still ring), zero padded to at least a third of a second so that peaks can be
    // placed finely enough to tell semitones apart in the bass.
    const size_t stepSamples = (size_t) std::max (1.0, stepSeconds * sampleRate);
    size_t n = 1024;
    while ((double) n < std::max ((double) stepSamples, sampleRate * 0.37))
        n <<= 1;
    const double binHz = sampleRate / (double) n;

    // First pass: the spectral peaks of every step, placed between bins by a parabola through
    // the three around each.
    std::vector<std::vector<Peak>> peaks (numSteps);
    std::vector<std::complex<float>> buffer (n);
    std::vector<float> magnitude (n / 2);
    for (size_t s = 0; s < numSteps; ++s)
    {
        const double startSec = (double) s * stepSeconds;
        const size_t trim = (size_t) (0.08 * (double) stepSamples);
        const size_t first = (size_t) (startSec * sampleRate) + trim;
        if (first >= numSamples)
            continue;
        const size_t length = std::min (stepSamples - 2 * trim, numSamples - first);
        if (length < 64)
            continue;

        std::fill (buffer.begin(), buffer.end(), std::complex<float> (0.0f, 0.0f));
        double energy = 0.0;
        for (size_t i = 0; i < length; ++i)
        {
            const float x = mono[first + i];
            energy += (double) x * (double) x;
            const double w = 0.5 - 0.5 * std::cos (2.0 * pi * ((double) i + 0.5) / (double) length);
            buffer[i] = { x * (float) w, 0.0f };
        }
        result.energy[s] = (float) std::sqrt (energy / (double) length);
        fft (buffer);
        float loudest = 0.0f;
        for (size_t k = 0; k < n / 2; ++k)
        {
            magnitude[k] = std::abs (buffer[k]);
            loudest = std::max (loudest, magnitude[k]);
        }
        if (! (loudest > 1.0e-9f))
            continue;

        for (size_t k = 2; k + 1 < n / 2; ++k)
        {
            const float m = magnitude[k];
            if (m < 0.01f * loudest || m < magnitude[k - 1] || m <= magnitude[k + 1])
                continue;
            const float a = std::log (magnitude[k - 1] + 1.0e-12f), b = std::log (m), c = std::log (magnitude[k + 1] + 1.0e-12f);
            const float denom = a - 2.0f * b + c;
            const double offset = denom < 0.0f ? 0.5 * (double) (a - c) / (double) denom : 0.0;
            const double f = ((double) k + offset) * binHz;
            if (f < 30.0 || f > 2200.0)
                continue;
            // Compressed, so loud notes do not drown the rest.
            peaks[s].push_back ({ 69.0 + 12.0 * std::log2 (f / 440.0), f, std::sqrt (m / loudest) });
        }
    }

    // Second pass: the peaks as pitch classes, measured against the recording's own tuning.
    const double tuning = estimateTuning (peaks);
    result.tuningCents = 100.0 * tuning;
    for (size_t s = 0; s < numSteps; ++s)
    {
        Chroma full {}, low {};
        for (const auto& p : peaks[s])
        {
            const double midi = p.midi - tuning;
            const double nearest = std::round (midi);
            if (std::abs (midi - nearest) > 0.4)
                continue; // between two notes: not part of the harmony
            const int pc = wrap ((int) nearest);
            if (p.hz >= 50.0)
                full[(size_t) pc] += p.level;
            if (p.hz < 200.0)
                low[(size_t) pc] += p.level * (float) (200.0 / std::max (p.hz, 40.0)); // the lowest note counts most
        }
        auto normalise = [] (Chroma& c)
        {
            float total = 0.0f;
            for (float v : c) total += v;
            if (total > 1.0e-9f)
                for (float& v : c) v /= total;
        };
        normalise (full);
        normalise (low);
        result.chroma[s] = full;
        result.bass[s] = low;
    }

    // Steps far quieter than the loudest one carry no harmony worth following.
    float loudestStep = 0.0f;
    for (float e : result.energy) loudestStep = std::max (loudestStep, e);
    for (size_t s = 0; s < numSteps; ++s)
        if (! (result.energy[s] > 0.01f * loudestStep))
        {
            result.chroma[s] = Chroma {};
            result.bass[s] = Chroma {};
        }
    return result;
}

namespace
{
// A state of the chord tracker: a candidate chord with one of its notes in the bass.
struct State
{
    size_t candidate;
    int bass;
    size_t bassIndex;   // 0 root, 1 third (or suspension), 2 fifth, 3 seventh
};

// A stretch of steps on one state.
struct Run
{
    size_t state, first, end;
};

// Everything about the audio that does not depend on the detail.
class Tracker
{
public:
    Tracker (const BeatChroma& b, Key k, double barBeats)
        : beats (b), key (k), beatsPerBar (barBeats), candidates (candidatesFor (k))
    {
        numSteps = beats.chroma.size();
        loop = beats.lengthBeats > tiny ? beats.lengthBeats : (double) numSteps * beats.stepBeats;
        perBeat = beats.stepBeats > tiny ? beats.stepBeats : 1.0;
        for (size_t c = 0; c < candidates.size(); ++c)
            for (size_t i = 0; i < candidates[c].bassChoices.size(); ++i)
                states.push_back ({ c, candidates[c].bassChoices[i], i });
        computeEmissions();
        computeNovelty();
    }

    // The chords of the loop at one detail setting, as runs of states. Detail is followed in
    // steps of a twentieth; of all the settings up to the one asked for, the one that finds the
    // most chords wins (the setting asked for when there is a tie), so more detail never gives
    // fewer chords.
    std::vector<Run> runsFor (double detail) const
    {
        const int top = (int) std::lround (std::min (std::max (detail, 0.0), 1.0) * detailLevels);
        std::vector<Run> best;
        for (int level = 0; level <= top; ++level)
        {
            auto runs = decode ((double) level / detailLevels);
            if (best.empty() || runs.size() >= best.size())
                best = std::move (runs);
        }
        return best;
    }

    Progression progressionOf (const std::vector<Run>& runs) const
    {
        Progression out;
        if (numSteps == 0)
        {
            ChordEvent e;
            e.chord = chordsInKey (key).front();
            e.start = 0.0;
            e.length = loop > tiny ? loop : beatsPerBar;
            e.confidence = 0.0f;
            out.push_back (e);
            return out;
        }
        for (const auto& run : runs)
        {
            const auto& st = states[run.state];
            ChordEvent e;
            e.chord = candidates[st.candidate].chord;
            e.chord.bass = st.bassIndex == 0 ? -1 : st.bass;
            e.start = (double) run.first * beats.stepBeats;
            e.length = std::min ((double) run.end * beats.stepBeats, loop) - e.start;
            e.confidence = confidenceOf (run);
            if (e.length > tiny)
                out.push_back (e);
        }
        if (! out.empty())
            out.back().length = loop - out.back().start;
        return out;
    }

    size_t stepCount() const { return numSteps; }

private:
    const BeatChroma& beats;
    const Key key;
    const double beatsPerBar;
    const std::vector<Candidate> candidates;
    std::vector<State> states;
    size_t numSteps = 0;
    double loop = 0.0, perBeat = 1.0;
    std::vector<std::vector<float>> emission;
    std::vector<bool> silent;
    std::vector<double> novelty;

    // Template fit plus the chord's prior plus how well the bass register agrees. An inversion
    // has to be heard in the bass to win, and the fifth or seventh in the bass more clearly than
    // the third: a bass line that visits the fifth is far more common than a 6/4 chord.
    void computeEmissions()
    {
        const size_t numStates = states.size();
        emission.assign (numSteps, std::vector<float> (numStates, 0.0f));
        silent.assign (numSteps, true);
        std::vector<float> fit (candidates.size());
        for (size_t t = 0; t < numSteps; ++t)
        {
            float total = 0.0f;
            for (float v : beats.chroma[t]) total += v;
            silent[t] = ! (total > 1.0e-6f);
            if (silent[t])
                continue; // all states equal: the chords around decide
            float bassTop = 0.0f;
            for (float v : beats.bass[t]) bassTop = std::max (bassTop, v);
            for (size_t c = 0; c < candidates.size(); ++c)
                fit[c] = cosine (beats.chroma[t], candidates[c].shape) + candidates[c].prior;
            for (size_t s = 0; s < numStates; ++s)
            {
                const auto& st = states[s];
                float bassTerm = 0.0f;
                if (bassTop > 1.0e-6f)
                    bassTerm = bassWeight * beats.bass[t][(size_t) st.bass] / bassTop;
                if (st.bassIndex > 0)
                {
                    const int interval = wrap (st.bass - candidates[st.candidate].chord.root);
                    bassTerm -= interval == 3 || interval == 4 ? thirdInBass : fifthInBass;
                }
                emission[t][s] = fit[st.candidate] + bassTerm;
            }
        }
    }

    // How much the sound changes from one step to the next (0 = not at all). A change of chord
    // is cheaper where the sound changes and dearer where it holds still.
    void computeNovelty()
    {
        novelty.assign (numSteps, 0.5);
        for (size_t t = 1; t < numSteps; ++t)
        {
            if (silent[t] || silent[t - 1])
                continue;
            const double full = 1.0 - (double) cosineOf (beats.chroma[t - 1], beats.chroma[t]);
            const double low = 1.0 - (double) cosineOf (beats.bass[t - 1], beats.bass[t]);
            novelty[t] = 0.75 * full + 0.25 * low;
        }
    }

    // Viterbi over (chord, bass) states. A change of chord costs lambda (set by the detail) times
    // where in the bar it falls times the novelty there. A change of bass note alone under the
    // same chord costs as much as a chord change on a beat even on the half bar, so a bass line
    // walking under one chord does not read as inversions; on a bar line it costs the same as a
    // chord change there. Emissions count per beat, so the detail means the same at any tempo.
    // A small bonus keeps the tonic when nothing else decides.
    std::vector<Run> decode (double detail) const
    {
        std::vector<Run> runs;
        if (numSteps == 0)
            return runs;
        const size_t numStates = states.size();
        const double lambda = changeCost (detail);
        std::vector<std::vector<int>> back (numSteps, std::vector<int> (numStates, -1));
        std::vector<double> score (numStates), next (numStates);
        const int tonicRoot = wrap (key.tonic);
        const Quality tonicQuality = key.minor ? Quality::Minor : Quality::Major;
        for (size_t s = 0; s < numStates; ++s)
        {
            const auto& cand = candidates[states[s].candidate];
            const bool tonic = cand.chord.root == tonicRoot && cand.chord.quality == tonicQuality && states[s].bassIndex == 0;
            score[s] = perBeat * (double) emission[0][s] + (tonic ? 1.0e-4 : 0.0);
        }
        for (size_t t = 1; t < numSteps; ++t)
        {
            const double weight = positionWeight ((double) t * beats.stepBeats, beatsPerBar);
            const double scale = lambda * (1.25 - 0.6 * std::min (1.0, novelty[t] / 0.3));
            const double chordCost = scale * weight;
            const double bassCost = scale * (weight > 1.0 ? std::max (weight, 2.4) : weight);
            // The best state, and the best one of any other chord.
            size_t best = 0;
            for (size_t s = 1; s < numStates; ++s)
                if (score[s] > score[best]) best = s;
            size_t other = numStates;
            for (size_t s = 0; s < numStates; ++s)
                if (states[s].candidate != states[best].candidate && (other == numStates || score[s] > score[other]))
                    other = s;
            for (size_t s = 0; s < numStates; ++s)
            {
                double value = score[s];
                size_t from = s;
                auto consider = [&] (size_t source, double cost)
                {
                    if (source < numStates && source != s && score[source] - cost > value)
                    {
                        value = score[source] - cost;
                        from = source;
                    }
                };
                if (states[s].candidate != states[best].candidate)
                    consider (best, chordCost);
                else
                {
                    consider (best, bassCost);
                    consider (other, chordCost);
                }
                next[s] = value + perBeat * (double) emission[t][s];
                back[t][s] = (int) from;
            }
            score.swap (next);
        }
        size_t current = 0;
        for (size_t s = 1; s < numStates; ++s)
            if (score[s] > score[current]) current = s;
        std::vector<size_t> path (numSteps);
        for (size_t t = numSteps; t-- > 0;)
        {
            path[t] = current;
            if (t > 0)
                current = (size_t) back[t][current];
        }

        for (size_t t = 0; t < numSteps;)
        {
            size_t end = t + 1;
            while (end < numSteps && path[end] == path[t]) ++end;
            runs.push_back ({ path[t], t, end });
            t = end;
        }
        mergeBassFigures (runs);
        return runs;
    }

    // A bass line that steps off the root for a beat and comes back (C C/G C) plays a figure under
    // one chord rather than changing it: such a stretch takes the chord around it. The loop wraps,
    // so the last run is followed by the first.
    void mergeBassFigures (std::vector<Run>& runs) const
    {
        if (runs.size() < 3)
            return;
        const double longest = 1.0 + tiny;
        for (size_t i = 0; i < runs.size(); ++i)
        {
            const Run& before = runs[(i + runs.size() - 1) % runs.size()];
            const Run& after = runs[(i + 1) % runs.size()];
            const Run& run = runs[i];
            if (before.state != after.state || before.state == run.state
                || states[run.state].candidate != states[before.state].candidate
                || (double) (run.end - run.first) * beats.stepBeats > longest)
                continue;
            runs[i].state = before.state;
        }
        std::vector<Run> merged;
        for (const auto& run : runs)
        {
            if (! merged.empty() && merged.back().state == run.state)
                merged.back().end = run.end;
            else
                merged.push_back (run);
        }
        runs.swap (merged);
    }

    // The average margin of the run's chord over the best different chord, 0.5 when they are
    // level, 0 for silence.
    float confidenceOf (const Run& run) const
    {
        const size_t candidate = states[run.state].candidate;
        double margin = 0.0;
        int counted = 0;
        for (size_t u = run.first; u < run.end && u < numSteps; ++u)
        {
            if (silent[u])
                continue;
            float other = -1.0e9f;
            for (size_t s = 0; s < states.size(); ++s)
                if (states[s].candidate != candidate)
                    other = std::max (other, emission[u][s]);
            margin += (double) (emission[u][run.state] - other);
            ++counted;
        }
        return counted == 0 ? 0.0f : (float) std::min (1.0, std::max (0.0, 0.5 + 4.0 * margin / counted));
    }
};
}

Progression detectChordTimeline (const BeatChroma& beats, Key key, double detail, double beatsPerBar)
{
    const Tracker tracker (beats, key, beatsPerBar > tiny ? beatsPerBar : 4.0);
    return tracker.progressionOf (tracker.runsFor (detail));
}

std::vector<Progression> suggestTimedIdeas (const BeatChroma& beats, Key key, double detail, int count,
                                            std::uint32_t seed, double beatsPerBar)
{
    std::vector<Progression> ideas;
    if (count <= 0)
        return ideas;
    const Tracker tracker (beats, key, beatsPerBar > tiny ? beatsPerBar : 4.0);
    const auto runs = tracker.runsFor (detail);
    const Progression detected = tracker.progressionOf (runs);
    ideas.push_back (detected);

    // What each detected chord's stretch of audio sounds like, for the alternatives to fit. The
    // chroma is summed, so a long chord weighs more than a passing one.
    std::vector<Chroma> segments;
    for (size_t i = 0; i < detected.size(); ++i)
    {
        Chroma sum {};
        if (i < runs.size())
            for (size_t s = runs[i].first; s < runs[i].end && s < beats.chroma.size(); ++s)
                for (size_t k = 0; k < 12; ++k)
                    sum[k] += beats.chroma[s][k];
        // Without audio the detected chord stands in for it.
        float total = 0.0f;
        for (float v : sum) total += v;
        if (! (total > 1.0e-6f))
            for (int pc : chordPitchClasses (detected[i].chord))
                sum[(size_t) pc] += (float) std::max (detected[i].length, 0.25);
        segments.push_back (sum);
    }

    // A new seed hears each stretch a little differently, which leads to other choices.
    // Where an alternative keeps the detected root, the detected chord stays as it was heard (with
    // its quality, 7th, suspension and bass note). Every idea keeps the detected timing, so a
    // substitute that would repeat the chord next to it is left out and the detected chord stays.
    const auto alternatives = suggestProgressions (segments, key, 3 * count + 3, seed);
    for (const auto& chords : alternatives)
    {
        if ((int) ideas.size() >= count)
            break;
        Progression idea = detected;
        for (size_t i = 0; i < idea.size() && i < chords.size(); ++i)
        {
            if (wrap (chords[i].root) == wrap (detected[i].chord.root))
                continue;
            const bool repeatsPrevious = i > 0 && idea[i - 1].chord == chords[i];
            const bool repeatsNext = i + 1 < idea.size() && idea[i + 1].chord == chords[i];
            if (repeatsPrevious || repeatsNext)
                continue;
            idea[i].chord = chords[i];
            idea[i].coloured = false;
            idea[i].confidence = 1.0f;
        }
        bool fresh = true;
        for (const auto& existing : ideas)
            fresh = fresh && chordsOf (existing) != chordsOf (idea);
        if (fresh)
            ideas.push_back (idea);
    }
    return ideas;
}
}
