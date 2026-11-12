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

double estimateTempo (const float* mono, size_t numSamples, double sampleRate)
{
    if (mono == nullptr || sampleRate <= 0.0)
        return 0.0;

    // Onset strength 200 times a second: how much louder each part of the spectrum got.
    // (A whole number of samples per frame: an uneven hop would put a rhythm of its own into it.)
    const size_t hop = std::max ((size_t) 1, (size_t) std::lround (sampleRate / 200.0));
    const double frameRate = sampleRate / (double) hop;
    size_t n = 256;
    while ((double) n < sampleRate * 0.02)
        n <<= 1;
    numSamples = std::min (numSamples, (size_t) (sampleRate * 60.0));
    if ((double) numSamples < sampleRate * 2.0 || numSamples < n)
        return 0.0;

    float peak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i)
        peak = std::max (peak, std::abs (mono[i]));
    if (! (peak > 1.0e-6f))
        return 0.0;
    const float gain = 1.0f / peak;

    std::vector<float> window (n);
    for (size_t i = 0; i < n; ++i)
        window[i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * (double) i / (double) n));

    const size_t numFrames = (numSamples - n) / hop + 1;
    const size_t numBins = n / 2;
    const float compression = 1000.0f / (float) n;
    std::vector<std::complex<float>> buffer (n);
    std::vector<float> previous (numBins, 0.0f), current (numBins), onset (numFrames, 0.0f);
    for (size_t f = 0; f < numFrames; ++f)
    {
        const size_t start = f * hop;
        for (size_t i = 0; i < n; ++i)
            buffer[i] = { mono[start + i] * gain * window[i], 0.0f };
        fft (buffer);
        float flux = 0.0f;
        for (size_t k = 1; k < numBins; ++k)
        {
            current[k] = std::log (1.0f + compression * std::abs (buffer[k]));
            flux += std::max (0.0f, current[k] - previous[k]);
        }
        if (f > 0)
            onset[f] = flux;
        previous.swap (current);
    }

    // Only real onsets count: the faint ripple a steady tone leaves is periodic too.
    float strongest = 0.0f;
    for (float v : onset)
        strongest = std::max (strongest, v);
    if (strongest < 0.01f * (float) numBins)
        return 0.0; // nothing ever starts
    for (auto& v : onset)
        v = std::max (0.0f, v - 0.08f * strongest);

    // Smooth a little so a click is a few frames wide, then remove the average.
    std::vector<float> envelope (numFrames);
    double mean = 0.0;
    for (size_t f = 0; f < numFrames; ++f)
    {
        const float before = f > 0 ? onset[f - 1] : 0.0f, after = f + 1 < numFrames ? onset[f + 1] : 0.0f;
        envelope[f] = 0.25f * before + 0.5f * onset[f] + 0.25f * after;
        mean += (double) envelope[f];
    }
    mean /= (double) numFrames;
    for (auto& v : envelope)
        v -= (float) mean;

    // Autocorrelation up to eight beats of the slowest tempo (or half the audio if it is short).
    const double shortestPeriod = frameRate * 60.0 / maxTempo, longestPeriod = frameRate * 60.0 / minTempo;
    const size_t maxLag = std::min ((size_t) (8.0 * longestPeriod) + 2, numFrames / 2);
    if ((double) maxLag < longestPeriod + 2.0)
        return 0.0;
    std::vector<double> acf (maxLag + 1, 0.0);
    for (size_t lag = 0; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        for (size_t f = 0; f + lag < numFrames; ++f)
            sum += (double) envelope[f] * (double) envelope[f + lag];
        acf[lag] = sum / (double) (numFrames - lag);
    }
    if (! (acf[0] > 1.0e-12))
        return 0.0;

    // How regular the onsets are at a beat period: the autocorrelation at the first `beats`
    // multiples of it, 1 for a perfect pulse.
    auto comb = [&] (double period, int beats)
    {
        double sum = 0.0;
        int used = 0;
        for (int beat = 1; beat <= beats; ++beat)
        {
            const double lag = period * (double) beat;
            const size_t below = (size_t) lag;
            if (below + 1 > maxLag)
                break;
            const double fraction = lag - (double) below;
            sum += acf[below] * (1.0 - fraction) + acf[below + 1] * fraction;
            ++used;
        }
        return used > 0 ? sum / (double) used / acf[0] : 0.0;
    };
    // One and two beats rule out twice the real tempo (its odd beats fall between the onsets).
    // Half the real tempo is as regular as the tempo itself, often more so (the snare comes
    // round every two beats), so a preference for tempos near 120 decides between the two.
    auto score = [&] (double period)
    {
        const double octaves = std::log2 (frameRate * 60.0 / period / 120.0);
        return comb (period, 2) * std::exp (-0.5 * octaves * octaves / 0.6);
    };
    // The best period within `reach` frames of `centre`, to a hundredth of a frame. Many beats
    // are lined up here: sixteen beats out, a small error in the period is plain to see.
    auto refine = [&] (double centre, double reach)
    {
        const int beats = std::min (16, std::max (4, (int) ((double) (maxLag - 1) / (centre + reach))));
        double best = centre, bestValue = comb (centre, beats);
        for (double width = reach; width > 0.005; width *= 0.2)
        {
            const double middle = best;
            for (int k = -5; k <= 5; ++k)
            {
                const double period = middle + (double) k * width * 0.2;
                if (period < shortestPeriod || period > longestPeriod)
                    continue;
                const double value = comb (period, beats);
                if (value > bestValue) { bestValue = value; best = period; }
            }
        }
        return best;
    };

    const double step = 0.125;
    double bestPeriod = 0.0, bestScore = 0.0;
    for (double period = shortestPeriod; period <= longestPeriod + 1.0e-9; period += step)
    {
        const double s = score (period);
        if (s > bestScore) { bestScore = s; bestPeriod = period; }
    }
    const double slowest = score (longestPeriod); // the steps above may stop just short of it
    if (slowest > bestScore) { bestScore = slowest; bestPeriod = longestPeriod; }
    // Nothing repeats (noise, a single held sound): chance alone gets this far.
    if (bestScore < std::max (0.04, 3.0 / std::sqrt ((double) numFrames)))
        return 0.0;

    bestPeriod = refine (bestPeriod, step);

    // A fast waltz can win as one and a half beats (two of those make its bar), which lands
    // every other beat between two real ones. When onsets two thirds of that apart are regular
    // over three steps, they are the beats. In 4/4 those lags fall between the onsets.
    const double twoThirds = bestPeriod * 2.0 / 3.0;
    if (twoThirds >= shortestPeriod * 0.98)
    {
        const double thirdsPeriod = refine (std::max (twoThirds, shortestPeriod), 1.0);
        if (comb (thirdsPeriod, 3) > 0.9 * comb (bestPeriod, 2))
            bestPeriod = thirdsPeriod;
    }

    // When the onsets half a beat apart are just as regular as the beats themselves, they are
    // the beats: an even pulse at 180 is 180, not 90 with something on every offbeat.
    const double half = bestPeriod * 0.5;
    if (half >= shortestPeriod * 0.98)
    {
        const double halfPeriod = refine (std::max (half, shortestPeriod), 1.0);
        const size_t nearest = (size_t) std::lround (halfPeriod);
        double between = 0.0;
        for (size_t lag = nearest - 1; lag <= nearest + 1; ++lag)
            between = std::max (between, acf[lag]);
        if (between / acf[0] > 0.9 * comb (bestPeriod, 2))
            bestPeriod = halfPeriod;
    }

    // Loops are nearly always made at a whole tempo: a result this close to one is that tempo.
    double tempo = frameRate * 60.0 / bestPeriod;
    if (std::abs (tempo - std::round (tempo)) < tempo * 0.0015)
        tempo = std::round (tempo);
    return std::min (std::max (tempo, minTempo), maxTempo);
}
}
