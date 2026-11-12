#pragma once
/*
  FilterNonlinear - building blocks of the nonlinear filter family (D5c, types 28-33).

  1. In-loop saturator  sat (b)  and its secant slope  secant (b) = sat (b) / b.
        sat (b) = b (27 + b^2) / (27 + 9 b^2)   for |b| < 3,   sign (b)  beyond.
     A tanh look-alike (small-signal slope exactly 1, C1 at |b| = 3, slope <= 1 everywhere, odd, bounded by 1) that costs one
     division.  The secant form lets the ladder solve its zero-delay feedback in closed form (see FilterStage.cpp).

  2. Double-precision half-band pair for the LOCAL 2x oversampling of the nonlinear cores.
        up2   : polyphase all-pass interpolator, n samples in -> 2n samples out   (even = direct branch, odd = delayed branch)
        down2 : polyphase all-pass decimator,   2n samples in -> n samples out    (0.5 (direct + delayed), unity DC gain)
     Same coefficients and branch assignment as the voice decimator (HalfbandIIR.h, design transition width 0.10, -105 dB:
     passband 0.4 fs, stopband from 0.6 fs of the BASE rate), run in double with fixed storage: no allocation, no locks.
     Latency is the group delay of the two filters (a few samples), not compensated (instrument).
*/

#include "DSP/HalfbandIIR.h"

#include <array>
#include <cmath>
#include <cstddef>

namespace warpr::nonlin
{
    inline double sat (double b) noexcept
    {
        const double a = std::abs (b);

        if (a >= 3.0)
            return b < 0.0 ? -1.0 : 1.0;

        return b * (27.0 + b * b) / (27.0 + 9.0 * b * b);
    }

    /** sat (b) / b, = 1 at the origin.  NaN input stays NaN (the stage's non-finite guard owns that). */
    inline double secant (double b) noexcept
    {
        const double a = std::abs (b);

        if (a >= 3.0)
            return 1.0 / a;

        return (27.0 + b * b) / (27.0 + 9.0 * b * b);
    }

    /** Diode-like asymmetric limiter used by Scream: positive side limits at +1, negative side at -0.6 (both with unit
        small-signal slope, slope <= 1 everywhere, so a feedback loop through it obeys the small-gain theorem). */
    inline double clipAsym (double x) noexcept
    {
        return x >= 0.0 ? sat (x) : 0.6 * sat (x * (1.0 / 0.6));
    }

    //==========================================================================
    struct HalfbandD
    {
        int numStages = 0;
        int directStages = 0;
        std::array<double, static_cast<std::size_t> (HalfbandCoeffs::maxStages)> alpha {};

        void load() noexcept
        {
            const HalfbandCoeffs& c = HalfbandCoeffs::get();
            numStages = c.numStages;
            directStages = c.directStages;

            for (std::size_t i = 0; i < alpha.size(); ++i)
                alpha[i] = static_cast<double> (c.alpha[i]);
        }
    };

    struct HalfbandStateD
    {
        std::array<double, static_cast<std::size_t> (HalfbandCoeffs::maxStages)> v {};
        double delay = 0.0;

        void reset() noexcept
        {
            v.fill (0.0);
            delay = 0.0;
        }
    };

    /** n samples in, 2n out (`out` must not alias `in`). */
    inline void up2 (const HalfbandD& c, HalfbandStateD& s, const double* in, double* out, int n) noexcept
    {
        const std::size_t total = static_cast<std::size_t> (c.numStages);
        const std::size_t direct = static_cast<std::size_t> (c.directStages);

        for (int i = 0; i < n; ++i)
        {
            double x = in[i];

            for (std::size_t k = 0; k < direct; ++k)
            {
                const double a = c.alpha[k];
                const double o = a * x + s.v[k];
                s.v[k] = x - a * o;
                x = o;
            }

            out[2 * i] = x;
            x = in[i];

            for (std::size_t k = direct; k < total; ++k)
            {
                const double a = c.alpha[k];
                const double o = a * x + s.v[k];
                s.v[k] = x - a * o;
                x = o;
            }

            out[2 * i + 1] = x;
        }
    }

    /** 2n samples in, n out (`out` must not alias `in`). */
    inline void down2 (const HalfbandD& c, HalfbandStateD& s, const double* in, double* out, int n) noexcept
    {
        const std::size_t total = static_cast<std::size_t> (c.numStages);
        const std::size_t direct = static_cast<std::size_t> (c.directStages);

        for (int i = 0; i < n; ++i)
        {
            double x = in[2 * i];

            for (std::size_t k = 0; k < direct; ++k)
            {
                const double a = c.alpha[k];
                const double o = a * x + s.v[k];
                s.v[k] = x - a * o;
                x = o;
            }

            const double directOut = x;
            x = in[2 * i + 1];

            for (std::size_t k = direct; k < total; ++k)
            {
                const double a = c.alpha[k];
                const double o = a * x + s.v[k];
                s.v[k] = x - a * o;
                x = o;
            }

            out[i] = 0.5 * (s.delay + directOut);
            s.delay = x;
        }
    }
}
