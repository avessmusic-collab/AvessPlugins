#pragma once
/*
  FastMath - libm-free elementary functions for the oscillator inner loops (architecture C5).

  Everything here is double precision, branch-light and allocation-free.  The accuracy targets
  are chosen so that the paired ADAA kernels (Adaa.h) stay consistent: the antiderivative F1 and
  the function f are both built from the SAME exp2/log2 pair, so F1' == f holds to the accuracy of
  these approximations (tested to 1e-6 relative in PhaseD2Tests).

    exp2   : range reduction to [-0.5, 0.5], Taylor to z^8 (relative error < 1e-9)
    log2   : mantissa folded to [sqrt(1/2), sqrt(2)), atanh series to s^11   (abs error < 1e-9)
    pow    : exp2(y * log2(x))  for x >= 0   (0 for x <= 0 and y > 0)
    sin2pi : sin(2*pi*c) for any c, folded to [-1/4, 1/4], odd polynomial to x^13 (abs error < 1e-9)

  Domain guards are the caller's job where noted (hot-path functions do not branch on NaN).
*/

#include <cstdint>
#include <cstring>

namespace warpr::fastmath
{
    /** 2^x.  Valid for |x| <= 1000 (clamped); returns a normal double. */
    inline double exp2 (double x) noexcept
    {
        if (x < -1000.0)
            return 0.0;
        if (x > 1000.0)
            x = 1000.0;

        // floor(x + 0.5) without a libm call: shift into the positive range so truncation == floor
        const double kf = static_cast<double> (static_cast<std::int64_t> (x + 1024.5)) - 1024.0;
        const double f = x - kf;                                   // [-0.5, 0.5]
        const double z = f * 0.6931471805599453;

        const double p = 1.0 + z * (1.0 + z * (0.5 + z * (1.0 / 6.0 + z * (1.0 / 24.0 + z * (1.0 / 120.0
                       + z * (1.0 / 720.0 + z * (1.0 / 5040.0 + z * (1.0 / 40320.0))))))));

        const std::uint64_t bits = static_cast<std::uint64_t> (static_cast<std::int64_t> (kf) + 1023) << 52;
        double scale;
        std::memcpy (&scale, &bits, sizeof (scale));
        return p * scale;
    }

    /** log2(x) for x > 1e-300 (the caller guarantees positivity). */
    inline double log2 (double x) noexcept
    {
        std::uint64_t bits;
        std::memcpy (&bits, &x, sizeof (bits));

        int e = static_cast<int> ((bits >> 52) & 0x7FF) - 1023;
        bits = (bits & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;      // mantissa in [1, 2)

        double m;
        std::memcpy (&m, &bits, sizeof (m));

        if (m > 1.4142135623730951)
        {
            m *= 0.5;
            ++e;
        }

        const double s = (m - 1.0) / (m + 1.0);
        const double s2 = s * s;
        const double ln = 2.0 * s * (1.0 + s2 * (1.0 / 3.0 + s2 * (1.0 / 5.0 + s2 * (1.0 / 7.0
                        + s2 * (1.0 / 9.0 + s2 * (1.0 / 11.0))))));

        return static_cast<double> (e) + ln * 1.4426950408889634;
    }

    /** x^y for x >= 0.  Returns 0 for x <= 1e-300 (exact for y > 0, the only case used here). */
    inline double pow (double x, double y) noexcept
    {
        if (x <= 1.0e-300)
            return 0.0;

        return exp2 (y * log2 (x));
    }

    /** sin(2 * pi * c), c in cycles, any magnitude below ~1e6. */
    inline double sin2pi (double c) noexcept
    {
        const double n = static_cast<double> (static_cast<std::int64_t> (c + 1.0e6 + 0.5)) - 1.0e6;
        double t = c - n;                                           // [-0.5, 0.5]

        if (t > 0.25)
            t = 0.5 - t;
        else if (t < -0.25)
            t = -0.5 - t;

        const double x = t * 6.283185307179586;                     // [-pi/2, pi/2]
        const double x2 = x * x;

        return x * (1.0 + x2 * (-1.0 / 6.0 + x2 * (1.0 / 120.0 + x2 * (-1.0 / 5040.0 + x2 * (1.0 / 362880.0
                  + x2 * (-1.0 / 39916800.0 + x2 * (1.0 / 6227020800.0)))))));
    }
}
