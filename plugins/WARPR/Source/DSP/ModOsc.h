#pragma once
/*
  ModOsc - the modulator of warp modes 54-59 (architecture C8, phase D6).

  Two kinds of source, selected by modOscSource:

    BASIC SHAPES (0 Sine, 1 Triangle, 2 Saw, 3 Square).  A phase accumulator phi (cycles, double) advances by
    `ratio * dTheta` per oscillator-rate sample (dTheta = the carrier's cycle-clock increment), so the modulator runs
    at ratio x the note frequency.  The value functions below are evaluated analytically from phi and the phase
    increment dphi, with a polyBLEP residual on every step (Saw, Square) and a polyBLAMP residual on every slope break
    (Triangle, and the slope breaks of the integrated Saw / Square), i.e. band-limited at the oscillator rate.

        shape (src, phi, dphi)      m(phi) in [-1, 1], sine-aligned: zero at phi = 0 and rising (Sine, Triangle, Saw);
                                    Square is +1 for phi in [0, 1/2)
        integral (src, phi, dphi)   M(phi) = the integral of m with the mean removed and the peak normalised to 1:
                                    Sine -cos(2 pi phi), Triangle 8 (u - 2u^2) / 8 (2u^2 - 3u + 1) with u = frac (phi + 3/4),
                                    Saw 6 (t^2 - t) + 1 with t = frac (phi + 1/2), Square the triangle 4 phi - 1 / 3 - 4 phi.
                                    dM/dphi = kappa m with kappa = 2 pi (Sine), 8, 6, 4.  FM uses M, everything else m.

    SAMPLE (4).  The loaded sample is read a second time at its own playhead (P_m = lo + ratio (P - lo), see
    WarpStage.h), through its own mip level for the modulator speed.  SampleMod turns that raw read into the two
    signals the modes need:
        m  the read value divided by a slow peak follower (instant attack, 2 s release, floor 0.1), |m| <= 1
        M  a leaky integral of m (20 Hz high-pass: leak = exp (-2 pi 20 / rate)), scaled by 2 pi dphi so that a tone at
           the nominal modulator frequency gives the same unit amplitude as the analytic shapes, then divided by its
           own peak follower (floor 0.1), |M| <= 1.
    (architecture: "leaky integrator, 20 Hz HP, then peak-normalised by a slow follower")

  Also here: the soft limiter used in the Self FM / Cross Mod feedback paths.

  All double precision, libm-free in the per-sample paths, allocation-free.
*/

#include "DSP/FastMath.h"

#include <algorithm>
#include <cmath>

namespace warpr::modosc
{
    /** x - floor (x), exact (floorFast shifts by 1e6 and so carries ~1e-10 of rounding, which showed up as 1e-10 overshoots of the
        shapes; std::floor is a single instruction anyway). */
    inline double frac (double x) noexcept { return x - std::floor (x); }

    /** polyBLEP residual in the "step of height 2" convention (the classic saw formula): t = phase since the
        discontinuity in [0, 1), dt = phase increment per sample. */
    inline double blepStd (double t, double dt) noexcept
    {
        if (dt <= 0.0)
            return 0.0;

        if (t < dt)
        {
            const double x = t / dt;
            return x + x - x * x - 1.0;
        }

        if (t > 1.0 - dt)
        {
            const double x = (t - 1.0) / dt;
            return x * x + x + x + 1.0;
        }

        return 0.0;
    }

    /** polyBLAMP residual of a unit slope change (per unit phase): multiply by (slope change) * dt. */
    inline double blampStd (double t, double dt) noexcept
    {
        if (dt <= 0.0)
            return 0.0;

        if (t < dt)
        {
            const double x = 1.0 - t / dt;
            return x * x * x * (1.0 / 6.0);
        }

        if (t > 1.0 - dt)
        {
            const double x = 1.0 + (t - 1.0) / dt;
            return x * x * x * (1.0 / 6.0);
        }

        return 0.0;
    }

    inline double clampStep (double dphi) noexcept { return std::min (std::abs (dphi), 0.25); }

    /** m(phi), see the header.  src outside 0..3 is treated as Sine. */
    inline double shape (int src, double phi, double dphi) noexcept
    {
        const double dt = clampStep (dphi);

        switch (src)
        {
            case 1:
            {
                const double u = frac (phi + 0.75);
                // slope -4 -> +4 at u = 1/2 (change +8) and +4 -> -4 at u = 0 (change -8)
                return 4.0 * std::abs (u - 0.5) - 1.0 - 8.0 * dt * blampStd (u, dt) + 8.0 * dt * blampStd (frac (u + 0.5), dt);
            }

            case 2:
            {
                const double t = frac (phi + 0.5);
                return 2.0 * t - 1.0 - blepStd (t, dt);
            }

            case 3:
            {
                const double t = frac (phi);
                const double sq = t < 0.5 ? 1.0 : -1.0;
                return sq + blepStd (t, dt) - blepStd (frac (t + 0.5), dt);
            }

            default:
                return fastmath::sin2pi (phi);
        }
    }

    /** M(phi), see the header. */
    inline double integral (int src, double phi, double dphi) noexcept
    {
        const double dt = clampStep (dphi);

        switch (src)
        {
            case 1:
            {
                const double u = frac (phi + 0.75);
                return u < 0.5 ? 8.0 * (u - 2.0 * u * u) : 8.0 * (2.0 * u * u - 3.0 * u + 1.0);
            }

            case 2:
            {
                const double t = frac (phi + 0.5);
                return 6.0 * (t * t - t) + 1.0 - 12.0 * dt * blampStd (t, dt);          // slope +6 -> -6 at the wrap
            }

            case 3:
            {
                const double t = frac (phi);
                const double tri = t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t;
                return tri + 8.0 * dt * blampStd (t, dt) - 8.0 * dt * blampStd (frac (t + 0.5), dt);
            }

            default:
                return -fastmath::cos2pi (phi);
        }
    }

    //==========================================================================
    /** Time constants of SampleMod for one oscillator rate (two exp calls: compute once per rate, not per sample). */
    struct Coefs
    {
        double decay = 0.9999, leak = 0.9999;
    };

    inline Coefs coefsFor (double rate) noexcept
    {
        Coefs c;
        const double r = std::max (rate, 1000.0);
        c.decay = std::exp (-1.0 / (2.0 * r));
        c.leak = std::exp (-6.283185307179586 * 20.0 / r);
        return c;
    }

    struct SampleMod
    {
        double integ = 0.0, peakX = 0.0, peakI = 0.0;

        void reset() noexcept { *this = SampleMod {}; }

        /** x = raw modulator read, dphi = nominal modulator phase increment (ratio x dTheta).  m, M out, both within +-1. */
        void process (double x, double dphi, const Coefs& c, double& m, double& M) noexcept
        {
            peakX = std::max (std::abs (x), peakX * c.decay);
            m = x / std::max (peakX, 0.1);

            integ = c.leak * integ + 6.283185307179586 * dphi * m;
            peakI = std::max (std::abs (integ), peakI * c.decay);
            M = integ / std::max (peakI, 0.1);
        }
    };

    //==========================================================================
    /** Smooth odd limiter, tanh-like: x (27 + x^2) / (27 + 9 x^2), exactly +-1 from |x| = 3 on (continuous in value and
        slope there).  NaN in, NaN out (the feedback guard screens that before). */
    inline double softLimit (double x) noexcept
    {
        if (x >= 3.0)
            return 1.0;

        if (x <= -3.0)
            return -1.0;

        const double x2 = x * x;
        return x * (27.0 + x2) / (27.0 + 9.0 * x2);
    }
}
