#pragma once
/*
  AmpWarp - amplitude-domain warps, V and VT families (architecture C4).

  V  : memoryless value shapers with a closed-form antiderivative -> first-order ADAA (Adaa.h).
       4 Asym Positive, 5 Asym Negative, 6 Asym Bidirectional, 11 Wave Stretch, 12 Wave Compress,
       13 Wave Expand.
  VT : value x function of the note-cycle clock theta (no ADAA needed: linear in x, and the
       theta-function is analytic so its edges are band-limited by construction, see below).
       8 Flip, 10 Wave Skew.

  Neutral element: makeCoeffs() returns the Identity kernel at a <= 0, and the VT gains are exactly
  1 / (x + 0)/(1 + 0) at a <= 0, so every mode is a bit-exact identity at warpAmount = 0.

  Per-mode constants (starting values for tuning; structure is the contract):

   Asym power law  f(x) = x^pPos (x > 0), -|x|^pNeg (x < 0);  F1 = |x|^(p+1)/(p+1) on each side.
     4 Asym Positive       pPos = 1 / (1 + 3 a (0.5 + A)),  pNeg = 1   (positive lobe expanded -> even harmonics)
     5 Asym Negative       mirror of 4 on the negative lobe
     6 Asym Bidirectional  pPos = 1 / (1 + 3 a A),  pNeg = 1 / (1 + 3 a B)
     DC from the asymmetry is removed by the voice DC blocker (5 Hz); no extra normalisation (|f(x)| <= max(|x|, 1)).

   Blended power laws (k = 0.2 + 0.8 A is the "knee / threshold blend": A = 0 keeps 80 % linear,
   A = 1 is the pure law):  y = (1 - k) x + k law(x)
     11 Wave Stretch   law = sgn(x) (1 - (1 - |x|)^p), p = 1 + 3 a, |x| <= 1 (flat beyond: peak flattening compander)
                       F1 (|x| <= 1) = |x| - (1 - (1 - |x|)^(p+1)) / (p+1);  beyond: F1(1) + (|x| - 1)
     12 Wave Compress  law = sgn(x) |x|^p,     p = 1 + 3 a      (low-level compression)
     13 Wave Expand    law = sgn(x) |x|^(1/p), p = 1 + 3 a      (low-level expansion; infinite slope at 0 is
                       tamed by the antiderivative - ADAA averages through the cusp)

   Flip  y = x * s(theta),  s = 1 - 2 window(theta):  the LAST fraction a of the cycle (shifted by A - 0.5
         cycles) is polarity-inverted.  Edges are smoothstep ramps of width w = max(0.1 B, 2 dTheta) cycles
         (B = 0 -> a 2-OS-sample edge: the intended hard polarity step, band-limited to ~1 base sample;
         true polyBLEP arrives with the D6 BLEP machinery).  a -> 1 blends smoothly to a constant inversion.
   Wave Skew  y = (x + k r(theta)) / (1 + k),  k = 2 a,  r = 2 theta^q - 1 (q = 2^(4 (A - 0.5)), A = 0.5 -> linear
         ramp), blended toward sin(2 pi theta) by B.  The sawtooth wrap (r: +1 -> -1) is a smoothstep ramp of
         width max(2 dTheta, 0) cycles ending at the wrap: the intended discontinuity, band-limited the same way.
*/

#include "DSP/Adaa.h"
#include "DSP/FastMath.h"

#include <algorithm>

namespace warpr::ampwarp
{
    //==========================================================================
    // Paired kernels (f, F1)
    //==========================================================================
    struct AsymPowerKernel
    {
        double pPos = 1.0, pNeg = 1.0;

        double f (double x) const noexcept
        {
            if (x > 0.0)
                return fastmath::pow (x, pPos);
            if (x < 0.0)
                return -fastmath::pow (-x, pNeg);
            return 0.0;
        }

        double F1 (double x) const noexcept
        {
            if (x > 0.0)
                return fastmath::pow (x, pPos + 1.0) / (pPos + 1.0);
            if (x < 0.0)
                return fastmath::pow (-x, pNeg + 1.0) / (pNeg + 1.0);
            return 0.0;
        }
    };

    /** y = (1-k) x + k * sgn(x) |x|^p   (Wave Compress p>1, Wave Expand p<1). */
    struct PowerLawKernel
    {
        double p = 1.0, k = 1.0;

        double f (double x) const noexcept
        {
            const double law = x > 0.0 ? fastmath::pow (x, p) : (x < 0.0 ? -fastmath::pow (-x, p) : 0.0);
            return (1.0 - k) * x + k * law;
        }

        double F1 (double x) const noexcept
        {
            const double ax = x < 0.0 ? -x : x;
            return (1.0 - k) * 0.5 * x * x + k * fastmath::pow (ax, p + 1.0) / (p + 1.0);
        }
    };

    /** y = (1-k) x + k * sgn(x) (1 - (1 - |x|)^p), flat at +-1 beyond |x| = 1   (Wave Stretch). */
    struct CompanderKernel
    {
        double p = 1.0, k = 1.0;

        double f (double x) const noexcept
        {
            const double ax = x < 0.0 ? -x : x;
            const double mag = ax >= 1.0 ? 1.0 : 1.0 - fastmath::pow (1.0 - ax, p);
            return (1.0 - k) * x + k * (x < 0.0 ? -mag : mag);
        }

        double F1 (double x) const noexcept
        {
            const double ax = x < 0.0 ? -x : x;
            const double law = ax >= 1.0 ? (1.0 - 1.0 / (p + 1.0)) + (ax - 1.0)
                                         : ax - (1.0 - fastmath::pow (1.0 - ax, p + 1.0)) / (p + 1.0);
            return (1.0 - k) * 0.5 * x * x + k * law;
        }
    };

    //==========================================================================
    // Coefficient selection for the V family
    //==========================================================================
    struct AmpCoeffs
    {
        enum class Type : unsigned char { Identity, Asym, Power, Compander };

        Type   type = Type::Identity;
        double p1 = 1.0, p2 = 1.0, k = 1.0;
    };

    /** V-family coefficients for `mode` (4,5,6,11,12,13); Identity for a <= 0 or any other mode. */
    inline AmpCoeffs makeCoeffs (int mode, double a, double A, double B) noexcept
    {
        AmpCoeffs c;

        if (a <= 0.0)
            return c;

        const double blend = 0.2 + 0.8 * A;

        switch (mode)
        {
            case 4:  c.type = AmpCoeffs::Type::Asym;  c.p1 = 1.0 / (1.0 + 3.0 * a * (0.5 + A)); c.p2 = 1.0; break;
            case 5:  c.type = AmpCoeffs::Type::Asym;  c.p1 = 1.0; c.p2 = 1.0 / (1.0 + 3.0 * a * (0.5 + A)); break;
            case 6:  c.type = AmpCoeffs::Type::Asym;  c.p1 = 1.0 / (1.0 + 3.0 * a * A); c.p2 = 1.0 / (1.0 + 3.0 * a * B); break;
            case 11: c.type = AmpCoeffs::Type::Compander; c.p1 = 1.0 + 3.0 * a; c.k = blend; break;
            case 12: c.type = AmpCoeffs::Type::Power; c.p1 = 1.0 + 3.0 * a; c.k = blend; break;
            case 13: c.type = AmpCoeffs::Type::Power; c.p1 = 1.0 / (1.0 + 3.0 * a); c.k = blend; break;
            default: break;
        }

        return c;
    }

    /** The nonlinear remainder g(x) = f(x) - x of a paired kernel, with G(x) = F1(x) - x^2/2.
        ADAA is applied to the REMAINDER only:  y = x + ADAA(g).  Plain ADAA of the identity part would
        be a two-sample average (a low-pass), which would make the stage audibly different from a bypass
        for arbitrarily small amounts; with the remainder form y -> x continuously as the amount -> 0.
        (Same construction as the MasterStage safety knee.) */
    template <class Kernel>
    struct RemainderKernel
    {
        const Kernel& k;

        double f (double x) const noexcept  { return k.f (x) - x; }
        double F1 (double x) const noexcept { return k.F1 (x) - 0.5 * x * x; }
    };

    template <class Kernel>
    inline double runRemainder (adaa::State& st, const Kernel& k, double x) noexcept
    {
        const RemainderKernel<Kernel> rem { k };
        return x + adaa::process (st, rem, x);
    }

    /** One ADAA step through the selected V-family kernel (updates `st`). */
    inline double run (const AmpCoeffs& c, adaa::State& st, double x) noexcept
    {
        switch (c.type)
        {
            case AmpCoeffs::Type::Asym:
            {
                const AsymPowerKernel k { c.p1, c.p2 };
                return runRemainder (st, k, x);
            }

            case AmpCoeffs::Type::Power:
            {
                const PowerLawKernel k { c.p1, c.k };
                return runRemainder (st, k, x);
            }

            case AmpCoeffs::Type::Compander:
            {
                const CompanderKernel k { c.p1, c.k };
                return runRemainder (st, k, x);
            }

            case AmpCoeffs::Type::Identity:
                break;
        }

        st.xPrev = x;
        st.valid = true;
        return x;
    }

    //==========================================================================
    // VT family: theta-driven gains
    //==========================================================================
    inline double smoothstep01 (double t) noexcept
    {
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        return t * t * (3.0 - 2.0 * t);
    }

    /** Flip (mode 8) gain s(theta) in [-1, 1]; exactly 1.0 at a <= 0. */
    inline double flipGain (double a, double A, double B, double theta, double dTheta) noexcept
    {
        if (a <= 0.0)
            return 1.0;

        const double w = std::min (0.5, std::max (1.0e-6, std::max (0.1 * B, 2.0 * dTheta)));
        const double aEff = std::min (a, 1.0 - w);
        const double inv = a > aEff ? std::min (1.0, (a - aEff) / w) : 0.0;

        // window: starts at s0 (last fraction aEff of the cycle, shifted by A - 0.5), soft rise / fall edges
        const double s0 = 1.0 - aEff + (A - 0.5);
        double d = theta - s0;
        d -= static_cast<double> (static_cast<long long> (d + 1024.0) - 1024LL);     // floor, d in [0, 1)

        const double rise = smoothstep01 (d / w);
        const double fall = smoothstep01 ((d - aEff) / w);
        const double gate = rise - fall;
        const double window = gate * (1.0 - inv) + inv;

        return 1.0 - 2.0 * window;
    }

    /** Wave Skew (mode 10) lean r(theta) in [-1, 1]. */
    inline double skewRamp (double A, double B, double theta, double dTheta) noexcept
    {
        const double q = fastmath::exp2 (4.0 * (A - 0.5));
        const double w = std::min (0.5, std::max (0.0, 2.0 * dTheta));

        double r;
        if (w > 0.0 && theta > 1.0 - w)
        {
            const double rEnd = 2.0 * fastmath::pow (1.0 - w, q) - 1.0;
            r = rEnd + (-1.0 - rEnd) * smoothstep01 ((theta - (1.0 - w)) / w);
        }
        else
        {
            r = 2.0 * fastmath::pow (theta, q) - 1.0;
        }

        if (B > 0.0)
            r += B * (fastmath::sin2pi (theta) - r);

        return r;
    }

    /** y = (x + k r) / (1 + k),  k = 2 a.  Exactly x at a <= 0. */
    inline double skewApply (double x, double a, double A, double B, double theta, double dTheta) noexcept
    {
        if (a <= 0.0)
            return x;

        const double k = 2.0 * a;
        return (x + k * skewRamp (A, B, theta, dTheta)) / (1.0 + k);
    }
}
