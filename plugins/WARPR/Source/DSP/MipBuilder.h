#pragma once
/*
  MipBuilder - band-limited octave pyramid for SampleData (architecture C1).

  Level k holds the source low-passed at 0.5 / 2^k cycles per source frame and decimated by 2^k.
  Each octave step is ONE 63-tap Kaiser half-band FIR (beta 10 -> about 100 dB stopband, passband
  edge about 0.2 cycles/sample, stopband edge about 0.3 cycles/sample at the stage input rate).
  Linear phase with the centre tap aligned to input frame 2m, so level-k frame m sits exactly at
  source frame m * 2^k (no position offset to compensate in the reader).

  Header-only, no JUCE dependency; runs on the loader / message thread only.
*/

#include <algorithm>
#include <cmath>

namespace warpr::mip
{
    /** Modified Bessel function of the first kind, order 0 (power series). */
    inline double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;

        for (int k = 1; k < 80; ++k)
        {
            term *= q / ((double) k * (double) k);
            sum += term;
            if (term < 1.0e-18 * sum)
                break;
        }

        return sum;
    }

    inline constexpr int    halfbandHalfLength = 31;      // 63 taps, centre tap at index 31
    inline constexpr int    halfbandOddTaps    = 16;      // taps at offsets +-1, +-3, ... +-31
    inline constexpr double halfbandKaiserBeta = 10.0;

    struct HalfbandTaps
    {
        double centre = 0.5;
        double odd[halfbandOddTaps] {};            // odd[q] = tap at offset (2q + 1), symmetric
    };

    inline HalfbandTaps designHalfband()
    {
        HalfbandTaps t;
        const double pi = 3.14159265358979323846;
        const double i0Beta = besselI0 (halfbandKaiserBeta);

        auto kaiser = [&] (int n)
        {
            const double r = (double) n / (double) halfbandHalfLength;
            return besselI0 (halfbandKaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
        };

        double sum = 0.5;                                          // centre tap (window value 1)

        for (int q = 0; q < halfbandOddTaps; ++q)
        {
            const int j = 2 * q + 1;
            const double sinc = ((q % 2) == 0 ? 1.0 : -1.0) / (pi * (double) j);   // sin(pi j / 2) / (pi j)
            t.odd[q] = sinc * kaiser (j);
            sum += 2.0 * t.odd[q];
        }

        t.centre = 0.5 / sum;                                      // unity DC gain
        for (double& v : t.odd)
            v /= sum;

        return t;
    }

    inline const HalfbandTaps& halfbandTaps()
    {
        static const HalfbandTaps taps = designHalfband();
        return taps;
    }

    /** One octave step: out[m] = sum_j h[j] * x[2m + j].  Circular input wraps (periodic single
        cycles), otherwise the input is zero-extended. */
    inline void decimateByTwo (const float* x, int n, bool circular, float* out, int outN)
    {
        const auto& t = halfbandTaps();

        auto at = [&] (int i) -> double
        {
            if (circular)
            {
                i %= n;
                if (i < 0)
                    i += n;
                return (double) x[i];
            }

            return (i < 0 || i >= n) ? 0.0 : (double) x[i];
        };

        for (int m = 0; m < outN; ++m)
        {
            const int c = 2 * m;
            double acc;

            if (c - halfbandHalfLength >= 0 && c + halfbandHalfLength < n)          // interior: no bounds logic
            {
                acc = t.centre * (double) x[c];
                for (int q = 0; q < halfbandOddTaps; ++q)
                {
                    const int j = 2 * q + 1;
                    acc += t.odd[q] * ((double) x[c - j] + (double) x[c + j]);
                }
            }
            else
            {
                acc = t.centre * at (c);
                for (int q = 0; q < halfbandOddTaps; ++q)
                {
                    const int j = 2 * q + 1;
                    acc += t.odd[q] * (at (c - j) + at (c + j));
                }
            }

            out[m] = (float) acc;
        }
    }
}
