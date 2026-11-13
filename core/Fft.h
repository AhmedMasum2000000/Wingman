// In-place radix-2 FFT shared by the analysis code. The size must be a power of two.
#pragma once
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

namespace backline
{
inline void fft (std::vector<std::complex<float>>& a)
{
    const double pi = 3.14159265358979323846;
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
}
