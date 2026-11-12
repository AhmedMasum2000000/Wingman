#include "Backline.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace backline
{
namespace
{
const double pi = 3.14159265358979323846;

void fft (std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = -2.0 * pi / (double) len;
        const std::complex<float> wLen ((float) std::cos (angle), (float) std::sin (angle));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<float> w (1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wLen;
            }
        }
    }
}

// Krumhansl-Kessler key profiles.
const float majorProfile[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
const float minorProfile[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

float correlation (const Chroma& chroma, const float* profile, int tonic)
{
    float meanC = 0, meanP = 0;
    for (int i = 0; i < 12; ++i) { meanC += chroma[(size_t) i]; meanP += profile[i]; }
    meanC /= 12.0f; meanP /= 12.0f;
    float num = 0, denC = 0, denP = 0;
    for (int i = 0; i < 12; ++i)
    {
        const float c = chroma[(size_t) ((i + tonic) % 12)] - meanC;
        const float p = profile[i] - meanP;
        num += c * p; denC += c * c; denP += p * p;
    }
    const float den = std::sqrt (denC * denP);
    return den > 0 ? num / den : 0.0f;
}

// Sum of squared differences between two runs of samples. Four separate partial sums let the
// additions overlap instead of each waiting for the one before, which is several times faster.
float squaredDifference (const float* a, const float* b, size_t n)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t j = 0;
    for (; j + 4 <= n; j += 4)
    {
        const float d0 = a[j] - b[j], d1 = a[j + 1] - b[j + 1], d2 = a[j + 2] - b[j + 2], d3 = a[j + 3] - b[j + 3];
        s0 += d0 * d0; s1 += d1 * d1; s2 += d2 * d2; s3 += d3 * d3;
    }
    for (; j < n; ++j)
    {
        const float d = a[j] - b[j];
        s0 += d * d;
    }
    return (s0 + s1) + (s2 + s3);
}

// YIN period search over one frame. Looks at lags from firstLag to lastLag and stops at the first
// dip of the normalised difference under the threshold, so a clear pitch costs little.
// Returns the period in samples, 0 if there is no dip in the range, or -1 if the dip is already
// under way at firstLag (the real period is shorter than the range covers).
// `frame` must have window + lastLag samples; `diff` must have lastLag + 1 elements.
double findPeriod (const float* frame, size_t window, size_t firstLag, size_t lastLag, std::vector<float>& diff)
{
    float running = 0;
    bool below = false;
    diff[0] = 1.0f;
    for (size_t tau = 1; tau <= lastLag; ++tau)
    {
        const float d = squaredDifference (frame, frame + tau, window);
        running += d;
        diff[tau] = running > 0 ? d * (float) tau / running : 1.0f;

        if (below && diff[tau] >= diff[tau - 1])
        {
            const size_t found = tau - 1;
            if (found == firstLag)
                return -1.0;
            const float a = diff[found - 1], b = diff[found], c = diff[found + 1];
            const float denom = a - 2 * b + c;
            return (double) found + (denom != 0 ? 0.5 * (a - c) / denom : 0.0);
        }
        if (! below && tau >= firstLag && tau < lastLag && diff[tau] < 0.15f)
            below = true;
    }
    return 0.0;
}
}

ChromaFrames computeChroma (const float* mono, size_t numSamples, double sampleRate)
{
    ChromaFrames result;
    // At least a quarter of a second: 16384 samples at both 44.1 and 48 kHz (bins under 3 Hz),
    // so neighbouring semitones stay apart down to the second octave at every standard rate.
    size_t n = 1024;
    while ((double) n < sampleRate * 0.25)
        n <<= 1;
    const size_t hop = n / 2;
    result.hopSeconds = (double) hop / sampleRate;
    result.windowSeconds = (double) n / sampleRate;
    if (numSamples < n)
        return result;

    std::vector<float> window (n);
    for (size_t i = 0; i < n; ++i)
        window[i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));

    // Map each FFT bin between 63.5 Hz (the lower edge of C2) and 2.1 kHz to a pitch class.
    struct Bin { size_t index; int pitchClass; float weight; };
    std::vector<Bin> bins;
    for (size_t k = 1; k < n / 2; ++k)
    {
        const double f = (double) k * sampleRate / (double) n;
        if (f < 63.5 || f > 2100.0)
            continue;
        const double midi = 69.0 + 12.0 * std::log2 (f / 440.0);
        const double nearest = std::round (midi);
        const float weight = (float) (1.0 - 2.0 * std::abs (midi - nearest));
        if (weight > 0.0f)
            bins.push_back ({ k, ((int) nearest % 12 + 12) % 12, weight });
    }

    std::vector<std::complex<float>> buffer (n);
    for (size_t start = 0; start + n <= numSamples; start += hop)
    {
        for (size_t i = 0; i < n; ++i)
            buffer[i] = { mono[start + i] * window[i], 0.0f };
        fft (buffer);
        Chroma c {};
        for (const auto& b : bins)
            c[(size_t) b.pitchClass] += std::abs (buffer[b.index]) * b.weight;
        result.frames.push_back (c);
    }
    return result;
}

Key detectKey (const ChromaFrames& chroma)
{
    Chroma sum {};
    for (const auto& f : chroma.frames)
        for (size_t i = 0; i < 12; ++i)
            sum[i] += f[i];

    Key best;
    float bestScore = -2.0f, secondScore = -2.0f;
    for (int tonic = 0; tonic < 12; ++tonic)
        for (int minor = 0; minor < 2; ++minor)
        {
            const float score = correlation (sum, minor ? minorProfile : majorProfile, tonic);
            if (score > bestScore)
            {
                secondScore = bestScore;
                bestScore = score;
                best.tonic = tonic;
                best.minor = minor != 0;
            }
            else if (score > secondScore)
                secondScore = score;
        }
    best.confidence = std::max (0.0f, bestScore - secondScore);
    return best;
}

std::vector<Chroma> segmentChroma (const ChromaFrames& chroma, double bpm, double beatsPerSegment, int numSegments)
{
    std::vector<Chroma> segments ((size_t) std::max (0, numSegments), Chroma {});
    const double segmentSeconds = beatsPerSegment * 60.0 / bpm;
    for (size_t i = 0; i < chroma.frames.size(); ++i)
    {
        const double centre = (double) i * chroma.hopSeconds + chroma.windowSeconds * 0.5;
        const int seg = (int) (centre / segmentSeconds);
        if (seg < 0 || seg >= numSegments)
            continue;
        for (size_t k = 0; k < 12; ++k)
            segments[(size_t) seg][k] += chroma.frames[i][k];
    }
    for (auto& s : segments)
    {
        float total = 0;
        for (float v : s) total += v;
        if (total > 1.0e-6f)
            for (float& v : s) v /= total;
    }
    return segments;
}

// Monophonic pitch tracking (YIN) on a decimated signal, grouped into notes.
// Tracks roughly E1 to C7 (41 Hz to 2.1 kHz).
std::vector<Note> audioToNotes (const float* mono, size_t numSamples, double sampleRate, double bpm)
{
    std::vector<Note> notes;
    const size_t factor = (size_t) std::max (1.0, std::floor (sampleRate / 22050.0));
    const double sr = sampleRate / (double) factor;
    std::vector<float> x (numSamples / factor);
    for (size_t i = 0; i < x.size(); ++i)
    {
        float acc = 0;
        for (size_t k = 0; k < factor; ++k) acc += mono[i * factor + k];
        x[i] = acc / (float) factor;
    }

    const size_t window = 1024, hop = 256;
    const size_t tauMin = (size_t) (sr / 2200.0), tauMax = (size_t) (sr / 38.0);
    if (x.size() < window + tauMax + 1)
        return notes;

    // Searching every lag at this rate is too slow for a long take with no clear pitch (chords,
    // noise), where nothing ends the search early. So only periods up to splitLag (notes above
    // about 550 Hz, which need the resolution) are looked for here; longer periods are looked for
    // in a half-rate copy, where the same search is four times cheaper.
    const size_t firstLag = std::max<size_t> (tauMin, 2);
    const size_t splitLag = std::min (tauMax, (size_t) (sr / 550.0));
    const size_t halfWindow = window / 2, halfFirstLag = std::max<size_t> (firstLag / 2, 2), halfLastLag = tauMax / 2;
    std::vector<float> half (x.size() / 2);
    for (size_t i = 0; i < half.size(); ++i)
        half[i] = 0.5f * (x[2 * i] + x[2 * i + 1]);

    std::vector<float> frameRms;
    for (size_t start = 0; start + window + tauMax < x.size(); start += hop)
    {
        float energy = 0;
        for (size_t j = 0; j < window; ++j) energy += x[start + j] * x[start + j];
        frameRms.push_back (std::sqrt (energy / (float) window));
    }

    // Gate relative to the loudest frame of the take (26 dB below it), so a quiet take is still
    // transcribed. Never above the old fixed 0.01, and never below -60 dBFS so hum is left alone.
    float loudest = 0;
    for (float v : frameRms) loudest = std::max (loudest, v);
    const float gate = std::min (0.01f, std::max (0.001f, 0.05f * loudest));

    std::vector<int> pitches (frameRms.size(), -1); // per frame, -1 = unvoiced
    std::vector<float> diff (std::max (splitLag, halfLastLag) + 1);
    for (size_t f = 0; f < pitches.size(); ++f)
    {
        if (frameRms[f] <= gate)
            continue;
        const size_t start = f * hop;
        double period = findPeriod (&x[start], window, firstLag, splitLag, diff);
        if (period == 0.0)
            period = 2.0 * findPeriod (&half[start / 2], halfWindow, halfFirstLag, halfLastLag, diff);
        if (period > 0.0)
            pitches[f] = (int) std::lround (69.0 + 12.0 * std::log2 (sr / period / 440.0));
    }

    // Short-term level at the centre of a frame, measured over `span` samples.
    auto level = [&] (size_t f, size_t span)
    {
        const float* centre = &x[f * hop + (window - span) / 2];
        float energy = 0;
        for (size_t j = 0; j < span; ++j) energy += centre[j] * centre[j];
        return std::sqrt (energy / (float) span);
    };
    // A new attack: the level jumps well above where it was over the last three frames.
    auto rising = [&level] (size_t f, size_t span)
    {
        return f >= 3 && level (f, span) > 1.5f * std::min ({ level (f - 1, span), level (f - 2, span), level (f - 3, span) });
    };

    const double beatsPerFrame = (double) hop / sr * bpm / 60.0;
    const size_t minFrames = 4, maxGap = 2;
    size_t i = 0;
    while (i < pitches.size())
    {
        if (pitches[i] < 0) { ++i; continue; }
        const int p = pitches[i];
        // Levels are measured over a whole number of the note's periods, about one hop long or
        // one period for low notes. Anything shorter rises and falls within each cycle of a bass
        // note and would look like a string of attacks.
        const double period = sr / (440.0 * std::pow (2.0, (p - 69) / 12.0));
        const size_t span = std::min (window, (size_t) std::lround (period * std::ceil ((double) hop / period)));
        size_t end = i + 1, gap = 0;
        bool wasRising = rising (i, span);
        for (size_t j = i + 1; j < pitches.size(); ++j)
        {
            const bool isRising = rising (j, span);
            if (j >= i + minFrames && isRising && ! wasRising) break; // same pitch struck again
            wasRising = isRising;
            if (pitches[j] == p) { end = j + 1; gap = 0; }
            else if (++gap > maxGap) break;
        }
        if (end - i >= minFrames)
            notes.push_back ({ p, (double) i * beatsPerFrame, (double) (end - i) * beatsPerFrame, 100 });
        i = end;
    }
    return notes;
}
}
