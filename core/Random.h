// A small deterministic random number generator (SplitMix64). The same seed gives the same
// numbers on every platform and compiler, which std:: distributions do not promise.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace backline
{
class Random
{
public:
    explicit Random (std::uint64_t seed) : state (seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}

    std::uint64_t next()
    {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // 0 <= result < 1
    double uniform() { return (double) (next() >> 11) * (1.0 / 9007199254740992.0); }
    // lo <= result < hi (lo when hi <= lo)
    int range (int lo, int hi) { return hi <= lo ? lo : lo + (int) (next() % (std::uint64_t) (hi - lo)); }
    bool chance (double probability) { return uniform() < probability; }
    // Index picked in proportion to the weights (non-positive weights are never picked unless all are).
    size_t weighted (const std::vector<double>& weights)
    {
        double total = 0.0;
        for (double w : weights)
            total += w > 0.0 ? w : 0.0;
        if (! (total > 0.0))
            return weights.empty() ? 0 : (size_t) range (0, (int) weights.size());
        double r = uniform() * total;
        for (size_t i = 0; i < weights.size(); ++i)
        {
            const double w = weights[i] > 0.0 ? weights[i] : 0.0;
            if (r < w)
                return i;
            r -= w;
        }
        for (size_t i = weights.size(); i-- > 0;)
            if (weights[i] > 0.0)
                return i;
        return 0;
    }

private:
    std::uint64_t state;
};

// Mixes several values into one seed.
inline std::uint64_t mixSeed (std::uint64_t a, std::uint64_t b)
{
    Random r (a ^ (b * 0xD6E8FEB86659FD93ull));
    return r.next();
}
}
