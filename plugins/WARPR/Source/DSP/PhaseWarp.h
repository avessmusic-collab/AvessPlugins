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

  ---- D6: phase & time (44-49), Phase Remapping (50), sync (51-53) ----------------------------------------------------
  The D6 maps are the same kind of object (g(theta) with g(0) = 0), plus three extras a single offset cannot express,
  which `evalMap` returns in a PhaseOut: a SECOND read tap (cross-fades at window edges, sub-cycle wraps and the BLEP of
  Hard Sync), and an output amplitude window.  Per-mode constants (starting values; the structure is the contract):

   44 Phase Distortion   CZ two-segment map, breakpoint d = 0.5 - 0.48 a, corner rounding A (0.5 A min (d, 1 - d));
                         resonance: c = 1 + 15 B, g = frac (c g0) and the window (1 - theta)^B on the output
   45 Phase Stretch      z = 1 - 0.95 a, s = A (1 - z), g = s + z theta; B > 0: the last 0.5 B of the cycle cross-fades
                         (equal power) with the read one window-length earlier, i.e. the material before the start s
   46 Phase Compression  c = 1 + 7 a, snapped toward round (c) by A, g = frac (c theta); B > 0: the last 0.5 B of every
                         COMPLETE sub-cycle cross-fades (equal power) into the next branch (g - 1)
   47 Phase Mirror       ping-pong g = tri (frac (k theta / 2)), k = 1 + 7 a half-cycles (k = 1 is the identity, a = 1 is the
                         architecture's c = 4), fractional k blending the adjacent integer maps; apex rounding B
   48 Phase Skew         g = theta + k [sin (2 pi (theta - phi)) + sin (2 pi phi)] / 2 pi + k2 [..2nd harmonic..] / 4 pi,
                         k = 0.95 a, phi = A - 0.5, k2 = 0.5 B (1 - k): strictly monotone
   49 Phase Quantization N = round (2^(10 - 9a)) steps, g = (floor (N theta) + q (frac (N theta))) / N, q a smoothstep
                         whose width is A (A = 1: the architecture's sm (frac); A -> 0: a hard staircase = the
                         intended discontinuity)
   50 Phase Remapping    g = lerp (theta, (curve (2 theta - 1) + 1) / 2, a), curve = the shaper's A/B curves blended by
                         A.  Evaluated as  Delta = a r (2 theta - 1) / 2  with r = f - x the stored REMAINDER, so the
                         identity curve (r == 0 exactly) is a bit-exact identity
   51 Windowed Sync      slave g = r theta, r = 1 + a (7 + 2 (A - 0.5)); output window 1 + a (sin (pi theta)^(0.25 + 3B) - 1)
   52 Hard Sync          the same slave map, reset at the master wrap.  B = 0: polyBLEP of the reset step (the 2-sample
                         residual is a cross-fade of the two trajectories: (1 - x)^2 / 2 after the event, (x + 1)^2 / 2
                         before it, x = distance in samples); B > 0: the last 0.15 B of the cycle smoothstep-fades into the
                         post-reset trajectory instead
   53 Soft Sync          the slave runs r theta in even master cycles and REFLECTS in odd ones: r (theta + rho (1 - 2 theta)),
                         rho = (1 - B) min (1, 10 a) (B = 0 full reverse = continuous; B = 1 = no reflection = Hard Sync)

  The modulation modes (54, 55, 58, 59) live in PhaseKernel.h (they need modulator / feedback state).
*/

#include "DSP/FastMath.h"
#include "DSP/ShaperEngine.h"

#include <algorithm>
#include <cmath>

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

    //--------------------------------------------------------------------------
    // D6 helpers
    inline double frac1 (double x) noexcept { return x - std::floor (x); }

    inline double smoothstep01 (double x) noexcept { return x <= 0.0 ? 0.0 : (x >= 1.0 ? 1.0 : x * x * (3.0 - 2.0 * x)); }

    /** Two-segment map with breakpoint d: [0, d] -> [0, 1/2], [d, 1] -> [1/2, 1]; C1 quadratic corner of half-width cw. */
    inline double twoSegment (double d, double cw, double theta) noexcept
    {
        const double s1 = 0.5 / d, s2 = 0.5 / (1.0 - d);
        const double u = theta - d;

        if (cw > 1.0e-9 && u > -cw && u < cw)
            return 0.5 + s1 * u + (s2 - s1) * (u + cw) * (u + cw) / (4.0 * cw);

        return u < 0.0 ? s1 * theta : 0.5 + s2 * u;
    }

    /** Slave / master ratio of the sync modes (1 at a = 0; A is a +-1 fine range). */
    inline double syncRatio (double a, double A) noexcept { return 1.0 + a * (7.0 + 2.0 * (A - 0.5)); }

    /** Phase Mirror (47) map: the ping-pong family m_k (theta) = tri (frac (k theta / 2)), k = 1 + 7 a half-cycles, blended
        between the adjacent integer counts.  Every integer m_k is continuous across the cycle wrap (modulo one cycle: m_k (1) is
        0 for even k and 1 for odd k) and m_1 (theta) = 1 - |theta - 1| = theta is the IDENTITY, so a = 0+ is neutral and the
        knob is continuous.  A FRACTIONAL k blends maps whose wrap endpoints differ by one cycle (odd k ends at 1, even k at 0), so the
        blend is not wrap-continuous (a topological fact: no continuous deformation of the identity into the ping-pong keeps
        g(1) - g(0) an integer); the row is flagged seamJump and the SeamBridge smooths the step.  Integer k are exact.
        (architecture: g = tri (frac (c theta)), c = 1 + 3a, i.e. k = 2c; the range is extended down to the
        identity k = 1 so that "neutral at a = 0" holds without a jump; a = 1 is the architecture's c = 4 = k = 8).
        Apex rounding B (softAbs half-width 0.5 B) also rounds the apex that sits on the cycle wrap for odd k, which leaves a
        small wrap step (SeamBridge, row flag seamJump). */
    inline double mirrorMap (double a, double B, double theta) noexcept
    {
        const double k = 1.0 + 7.0 * a;
        const double kf = std::floor (k);
        const double f = k - kf;
        const double cw = 0.5 * B;

        auto tri = [cw] (double x) noexcept
        {
            const double u = frac1 (x);
            return 1.0 - softAbs (2.0 * u - 1.0, cw);
        };

        double g = tri (0.5 * kf * theta);

        if (f > 0.0)
            g += f * (tri (0.5 * (kf + 1.0) * theta) - g);

        return g;
    }

    /** Phase Skew (48) map. */
    inline double skewMap (double a, double A, double B, double theta) noexcept
    {
        const double k = 0.95 * a, phi = A - 0.5;
        const double k2 = 0.5 * B * (1.0 - k);
        const double t = theta - phi;

        return theta + k * (fastmath::sin2pi (t) + fastmath::sin2pi (phi)) * (1.0 / 6.283185307179586)
                     + k2 * (fastmath::sin2pi (2.0 * t) + fastmath::sin2pi (2.0 * phi)) * (1.0 / 12.566370614359172);
    }

    /** Phase Quantization (49) map. */
    inline double quantMap (double a, double A, double theta) noexcept
    {
        const double n = std::max (2.0, std::floor (std::exp2 (10.0 - 9.0 * a) + 0.5));
        const double x = n * theta;
        const double fl = std::floor (x);
        const double u = x - fl;

        // smoothness ~0 is an EXACT staircase (every output a multiple of 1/N); the smoothstep
        // transition would otherwise leave off-grid samples inside its (tiny) width.
        if (A <= 1.0e-4)
            return (fl + (u >= 0.5 ? 1.0 : 0.0)) / n;

        return (fl + smoothstep01 ((u - 0.5) / A + 0.5)) / n;
    }

    /** Cycle-phase map g(theta) for mode `mode` (0..14 phase modes, 44-49, 51-53 with an even master cycle); identity
        for anything else (Phase Remapping needs the curve: see evalMap). */
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

            case 44:
            {
                const double d = 0.5 - 0.48 * a;
                const double g0 = twoSegment (d, 0.5 * A * std::min (d, 1.0 - d), theta);

                if (B <= 0.0)
                    return g0;

                const double v = (1.0 + 15.0 * B) * g0;
                return v - fastmath::floorFast (v);
            }

            case 45:
            {
                const double z = 1.0 - 0.95 * a;
                return A * (1.0 - z) + z * theta;
            }

            case 46:
            {
                const double c = 1.0 + 7.0 * a;
                const double cEff = c + A * (fastmath::floorFast (c + 0.5) - c);
                return frac1 (cEff * theta);
            }

            case 47:
                return mirrorMap (a, B, theta);

            case 48:
                return skewMap (a, A, B, theta);

            case 49:
                return quantMap (a, A, theta);

            case 51:
            case 52:
            case 53:
                return syncRatio (a, A) * theta;

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

    //==========================================================================
    // D6: the general per-sample result of a phase mode
    //==========================================================================
    /** Per-sample context of the map modes. */
    struct MapCtx
    {
        const ShaperLutSet* curve = nullptr;     // Phase Remapping: the shaper curves (nullptr = identity curve)
        int  parity = 0;                         // Soft Sync: master cycles elapsed, mod 2
        bool blep = true;                        // Hard Sync: polyBLEP of the reset step (off only for the BLEP test)
    };

    /** What a phase mode asks the oscillator to do for ONE sample:
            y = amp * (g0 * read (P + dir Lc d0) + g1 * read (P + dir Lc d1))
        A single-tap mode leaves d1 / g1 at 0 and amp at 1; `identity()` is then true exactly when the read is the plain
        read (the bit-exact path). */
    struct PhaseOut
    {
        double d0 = 0.0, d1 = 0.0;
        float  g0 = 1.0f, g1 = 0.0f;
        float  amp = 1.0f;

        bool identity() const noexcept
        {
            return juce::exactlyEqual (d0, 0.0) && g1 <= 0.0f && juce::exactlyEqual (amp, 1.0f);
        }
    };

    inline void setEqualPower (PhaseOut& o, double w) noexcept
    {
        const double wc = std::min (1.0, std::max (0.0, w));
        o.g0 = static_cast<float> (fastmath::cos2pi (0.25 * wc));
        o.g1 = static_cast<float> (fastmath::sin2pi (0.25 * wc));
    }

    inline void setLinear (PhaseOut& o, double w) noexcept
    {
        const double wc = std::min (1.0, std::max (0.0, w));
        o.g1 = static_cast<float> (wc);
        o.g0 = static_cast<float> (1.0 - wc);
    }

    /** Phase modes 0-14, 44-53 (the map modes).  Exactly the neutral PhaseOut at a <= 0.  For modes 0-14 the result is
        bit-identical to delta (single tap). */
    inline PhaseOut evalMap (int mode, double a, double A, double B, double theta, double dTheta, const MapCtx& cx) noexcept
    {
        PhaseOut o;

        if (a <= 0.0)
            return o;

        switch (mode)
        {
            case 44:
            {
                o.d0 = phaseMap (44, a, A, B, theta, dTheta) - theta;

                if (B > 0.0)
                    o.amp = static_cast<float> (fastmath::pow (1.0 - theta, B));

                return o;
            }

            case 45:
            {
                const double z = 1.0 - 0.95 * a;
                const double g = A * (1.0 - z) + z * theta;
                o.d0 = g - theta;

                const double we = 0.5 * B;

                if (we > 1.0e-6 && theta > 1.0 - we)
                {
                    o.d1 = (g - z) - theta;                        // the material one window-length before the read
                    setEqualPower (o, (theta - (1.0 - we)) / we);
                }

                return o;
            }

            case 46:
            {
                const double c = 1.0 + 7.0 * a;
                const double cEff = c + A * (fastmath::floorFast (c + 0.5) - c);
                const double u = cEff * theta;
                const double fl = fastmath::floorFast (u);
                const double g = u - fl;
                o.d0 = g - theta;

                const double we = 0.5 * B;

                if (we > 1.0e-6 && g > 1.0 - we && fl + 1.0 <= cEff + 1.0e-9)      // only sub-cycles that really complete
                {
                    o.d1 = (g - 1.0) - theta;                      // the next branch
                    setEqualPower (o, (g - (1.0 - we)) / we);
                }

                return o;
            }

            case 50:
            {
                if (cx.curve != nullptr)
                    o.d0 = 0.5 * a * cx.curve->rBlend (std::min (1.0, std::max (0.0, A)), 2.0 * theta - 1.0);

                return o;
            }

            case 51:
            {
                o.d0 = (syncRatio (a, A) - 1.0) * theta;

                const double win = fastmath::pow (fastmath::sin2pi (0.5 * theta), 0.25 + 3.0 * B);
                o.amp = static_cast<float> (1.0 + a * (win - 1.0));
                return o;
            }

            case 52:
            {
                const double r = syncRatio (a, A);
                o.d0 = (r - 1.0) * theta;

                const double softW = 0.15 * B;

                if (softW > 1.0e-4)
                {
                    if (theta > 1.0 - softW)
                    {
                        o.d1 = r * (theta - 1.0) - theta;          // the post-reset trajectory, extended backwards
                        setLinear (o, smoothstep01 ((theta - (1.0 - softW)) / softW));
                    }
                }
                else if (cx.blep && dTheta > 0.0 && dTheta < 0.25)
                {
                    if (theta < dTheta)                            // just after the reset: pull toward the pre-reset trajectory
                    {
                        const double x = theta / dTheta;
                        o.d1 = r * (theta + 1.0) - theta;
                        setLinear (o, 0.5 * (1.0 - x) * (1.0 - x));
                    }
                    else if (theta > 1.0 - dTheta)                 // just before it: pull toward the post-reset trajectory
                    {
                        const double x = (theta - 1.0) / dTheta;
                        o.d1 = r * (theta - 1.0) - theta;
                        setLinear (o, 0.5 * (x + 1.0) * (x + 1.0));
                    }
                }

                return o;
            }

            case 53:
            {
                const double r = syncRatio (a, A);
                const double rho = (1.0 - B) * std::min (1.0, 10.0 * a);
                const double g = (cx.parity & 1) != 0 ? r * (theta + rho * (1.0 - 2.0 * theta)) : r * theta;
                o.d0 = g - theta;
                return o;
            }

            default:
                o.d0 = phaseMap (mode, a, A, B, theta, dTheta) - theta;      // 0-14, 47, 48, 49 (and identity elsewhere)
                return o;
        }
    }
}
