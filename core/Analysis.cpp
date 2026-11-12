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
}

ChromaFrames computeChroma (const float* mono, size_t numSamples, double sampleRate)
{
    ChromaFrames result;
    size_t n = 1024;
    while ((double) n < sampleRate * 0.18)
        n <<= 1;
    const size_t hop = n / 2;
    result.hopSeconds = (double) hop / sampleRate;
    result.windowSeconds = (double) n / sampleRate;
    if (numSamples < n)
        return result;

    std::vector<float> window (n);
    for (size_t i = 0; i < n; ++i)
        window[i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));

    // Map each FFT bin between 65 Hz and 2.1 kHz to a pitch class.
    struct Bin { size_t index; int pitchClass; float weight; };
    std::vector<Bin> bins;
    for (size_t k = 1; k < n / 2; ++k)
    {
        const double f = (double) k * sampleRate / (double) n;
        if (f < 65.0 || f > 2100.0)
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
std::vector<Note> audioToNotes (const float* mono, size_t numSamples, double sampleRate, double bpm)
{
    std::vector<Note> notes;
    const size_t factor = (size_t) std::max (1.0, std::floor (sampleRate / 11025.0));
    const double sr = sampleRate / (double) factor;
    std::vector<float> x (numSamples / factor);
    for (size_t i = 0; i < x.size(); ++i)
    {
        float acc = 0;
        for (size_t k = 0; k < factor; ++k) acc += mono[i * factor + k];
        x[i] = acc / (float) factor;
    }

    const size_t window = 512, hop = 128;
    const size_t tauMin = (size_t) (sr / 1000.0), tauMax = (size_t) (sr / 70.0);
    if (x.size() < window + tauMax + 1)
        return notes;

    std::vector<int> pitches; // per frame, -1 = unvoiced
    std::vector<float> diff (tauMax + 1);
    for (size_t start = 0; start + window + tauMax < x.size(); start += hop)
    {
        float energy = 0;
        for (size_t j = 0; j < window; ++j) energy += x[start + j] * x[start + j];
        int pitch = -1;
        if (std::sqrt (energy / (float) window) > 0.01f)
        {
            float running = 0;
            diff[0] = 1.0f;
            size_t found = 0;
            for (size_t tau = 1; tau <= tauMax; ++tau)
            {
                float d = 0;
                for (size_t j = 0; j < window; ++j)
                {
                    const float delta = x[start + j] - x[start + j + tau];
                    d += delta * delta;
                }
                running += d;
                diff[tau] = running > 0 ? d * (float) tau / running : 1.0f;
            }
            for (size_t tau = std::max<size_t> (tauMin, 2); tau < tauMax; ++tau)
                if (diff[tau] < 0.15f)
                {
                    while (tau + 1 < tauMax && diff[tau + 1] < diff[tau]) ++tau;
                    found = tau;
                    break;
                }
            if (found > 0)
            {
                const float a = diff[found - 1], b = diff[found], c = diff[found + 1];
                const float denom = a - 2 * b + c;
                const double refined = (double) found + (denom != 0 ? 0.5 * (a - c) / denom : 0.0);
                pitch = (int) std::lround (69.0 + 12.0 * std::log2 (sr / refined / 440.0));
            }
        }
        pitches.push_back (pitch);
    }

    const double beatsPerFrame = (double) hop / sr * bpm / 60.0;
    const size_t minFrames = 4, maxGap = 2;
    size_t i = 0;
    while (i < pitches.size())
    {
        if (pitches[i] < 0) { ++i; continue; }
        const int p = pitches[i];
        size_t end = i + 1, gap = 0;
        for (size_t j = i + 1; j < pitches.size(); ++j)
        {
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
