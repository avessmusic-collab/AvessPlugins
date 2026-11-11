#pragma once
/*
  PhaseWarp - H family (phase-map bends), architecture C3 / section C+D.

  The unified phase-warp mechanism: a mode defines a cycle-phase map g(theta) with g(0) = 0 and
  (for continuous modes) g(1) = 1, where theta in [0, 1) is the per-oscillator note-cycle clock.  The
  oscillator reads the sample at

        P' = fold( P + dir * Lc * Delta ),      Delta = g(theta) - theta        (cycles of the root period Lc)

  This header supplies `delta(mode, a, A, B, theta, dTheta)`.  `a` = warpAmount (0 = neutral),
  A = warpParamA, B = warpParamB, all 0..1.  dTheta is the cycle-clock increment per OS sample (used
  only to keep corners at least about one OS sample wide).  delta() returns EXACTLY 0.0 when a <= 0,
  which makes the warp stage a bit-exact identity at warpAmount = 0 for every mode.

  Per-mode constants (all starting values for listening sessions; the STRUCTURE is the contract):

   e = 1 + a * (1 + 7 A)                                   exponent; A = 0.5 (default) -> e = 1 + 4.5 a (up to 5.5)

   0 Bend Positive      g = theta^e                          slow start, fast end
   1 Bend Negative      g = 1 - (1 - theta)^e                fast start, slow end
   2 Bend Symmetric     g = 0.5 (2 theta)^e  (theta < 0.5);  g = 1 - 0.5 (2 - 2 theta)^e otherwise
   3 Bend Asymmetric    first half exponent e (A), second half e2 = 1 + 8 a B (B)
   7 PWM                window w = 1 - 0.95 a (cycle compressed into w), offset o = A (1 - w):
                        g = clamp((theta - o) / w, 0, 1); outside the window the read position HOLDS.
                        Corner half-width (window units) c = max(0.25 B, dTheta / w), capped at 0.5 and at the
                        room between window and cycle boundary (keeps g(0) = 0, g(1) = 1 exact), C1 corners.
   9 Mirror             m = 1 - 0.5 a;  g = m - softAbs(theta - m, cw),  cw = min(0.25 B, 0.5 a)   (tail read backwards)
                        g(1) = 2m - 1 = 1 - a != 1  ->  SeamBridge splits the output step at the cycle wrap
   14 Variable Duty     breakpoint d = clamp(0.5 - 0.45 a + 0.25 a (2B - 1), 0.04, 0.96); first half of the
                        sample cycle is read in [0, d], the second half in [d, 1]; quadratic corner rounding
                        of width cw = 0.5 A min(d, 1 - d)  (A = 0.5 default -> 0.25 min(d, 1-d)).
*/

#include "DSP/FastMath.h"

#include <algorithm>

namespace warpr::phasewarp
{
    inline double clamp01 (double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    /** max(u, 0) with a C1 quadratic corner of half-width c (exact outside the corner). */
    inline double softMax0 (double u, double c) noexcept
    {
        if (c <= 1.0e-9)
            return u > 0.0 ? u : 0.0;
        if (u <= -c)
            return 0.0;
        if (u >= c)
            return u;

        const double v = u + c;
        return v * v / (4.0 * c);
    }

    /** |u| with a C1 quadratic corner of half-width c. */
    inline double softAbs (double u, double c) noexcept
    {
        const double au = u < 0.0 ? -u : u;

        if (c <= 1.0e-9 || au >= c)
            return au;

        return (u * u / c + c) * 0.5;
    }

    /** Cycle-phase map g(theta) for mode `mode` (0..14 phase modes); identity for anything else. */
    inline double phaseMap (int mode, double a, double A, double B, double theta, double dTheta) noexcept
    {
        const double e = 1.0 + a * (1.0 + 7.0 * A);

        switch (mode)
        {
            case 0:
                return fastmath::pow (theta, e);

            case 1:
                return 1.0 - fastmath::pow (1.0 - theta, e);

            case 2:
                return theta < 0.5 ? 0.5 * fastmath::pow (2.0 * theta, e)
                                   : 1.0 - 0.5 * fastmath::pow (2.0 - 2.0 * theta, e);

            case 3:
            {
                const double e2 = 1.0 + 8.0 * a * B;
                return theta < 0.5 ? 0.5 * fastmath::pow (2.0 * theta, e)
                                   : 1.0 - 0.5 * fastmath::pow (2.0 - 2.0 * theta, e2);
            }

            case 7:
            {
                const double w = 1.0 - 0.95 * a;
                const double o = A * (1.0 - w);
                const double u = (theta - o) / w;

                // corner half-width: B softness (>= about one OS sample), but never wider than the room
                // between the window and the cycle boundaries, so that g(0) = 0 and g(1) = 1 stay exact
                const double room = (1.0 - w) * std::min (A, 1.0 - A) / w;
                const double c = std::min (std::min (0.5, room), std::max (0.25 * B, dTheta / w));

                // smooth clamp(u, 0, 1):  min(max(u, 0), 1) = 1 - max(1 - max(u, 0), 0)
                return 1.0 - softMax0 (1.0 - softMax0 (u, c), c);
            }

            case 9:
            {
                // apex rounding never wider than the reflected tail (length 0.5 a): g(1) = 2m - 1 stays exact
                const double m = 1.0 - 0.5 * a;
                return m - softAbs (theta - m, std::min (0.25 * B, 0.5 * a));
            }

            case 14:
            {
                double d = 0.5 - 0.45 * a + 0.25 * a * (2.0 * B - 1.0);
                d = std::min (0.96, std::max (0.04, d));

                const double s1 = 0.5 / d, s2 = 0.5 / (1.0 - d);
                const double cw = 0.5 * A * std::min (d, 1.0 - d);
                const double u = theta - d;

                if (cw > 1.0e-9 && u > -cw && u < cw)
                    return 0.5 + s1 * u + (s2 - s1) * (u + cw) * (u + cw) / (4.0 * cw);

                return u < 0.0 ? s1 * theta : 0.5 + s2 * u;
            }

            default:
                return theta;
        }
    }

    /** Read-position offset in cycles of the root period:  g(theta) - theta.  Exactly 0 at a <= 0. */
    inline double delta (int mode, double a, double A, double B, double theta, double dTheta) noexcept
    {
        if (a <= 0.0)
            return 0.0;

        return phaseMap (mode, a, A, B, theta, dTheta) - theta;
    }
}
