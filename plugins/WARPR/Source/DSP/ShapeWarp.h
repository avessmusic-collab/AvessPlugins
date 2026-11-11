#pragma once
/*
  ShapeWarp - folds, dynamics, distortions, rectifiers and quantisers (warpMode 15-43), phase D3.

  Families: FOLD 15-22,24 | DYN 23 | CLIP 25-37 | RECT 38-40 | QNT 41-43.  All are amplitude-domain
  value shapers that run at the oversampled rate on the value read from the sample.

  STATIC-SHAPER WRAPPER (architecture "Static-shaper wrapper"):

      u      = D x + b                       D = 1 + a (1 + 15 A)    (a = warpAmount, A = warpParamA, B = warpParamB)
      s(x)   = nrm * (N(u) - N(b))           N = the mode's base nonlinearity, N(b) removes the offset at the origin
      y      = x + g (s(x) - x)              g = min(1, 20 a)  (rectifiers: g = a)

  g = 0 at a = 0, so every mode returns the input BIT-EXACTLY at warpAmount = 0 (and `run` early-outs).
  The ramp 20 a reaches 1 at a = 0.05.

  ANTI-ALIASING: every mode with a closed-form antiderivative provides the PAIR (f, F1):
      Kernel::f (x)  = s(x)                        Kernel::F1 (x) = nrm * (NI(u) / D - N(b) x)     NI' = N
  and is evaluated with first-order ADAA in REMAINDER form (ampwarp::runRemainder: x + ADAA(s - x), Simpson
  mean below |dx| 1e-5).  Exceptions, documented per mode below: 24 Chebyshev Fold (Simpson-quadrature
  "numerical ADAA", no closed form for real order), 42 SR Reduction (sample & hold: aliases by design),
  41 Bitcrush is ADAA'd on its staircase but flagged alias-by-design (quantisation distortion is the effect).

  DC: odd shapers have none.  Asymmetric / rectifying shapers subtract N(b) (analytic offset at the origin, so
  silence stays silence); the remaining mean is removed by the voice DC blocker (5 Hz).

  PER-MODE CONSTANTS (starting values for tuning; structure is the contract).  u = D x + b, T(.) = triangle fold
  (period 4, slope +-1, corners at odd integers), tf, TI = its antiderivative.

   15 Linear Fold      N = tf(u)                          b = B/2 (pre-offset)              nrm 1
   16 Multi Fold       K = 2 + round(3B) cascaded triangle folds with thresholds T_j = 2^-j (each stage ADAA'd on
                       its own input; stage output |v| <= T_j), output * 2^((K-1)/2) (half-compensation for the
                       halving range: small-signal gain 2^((K-1)/2) D, large-signal peak 2^(-(K-1)/2))
   17 Sine Fold        N = (sin(pi/2 u) + 0.3B sin(pi u)) / (1 + 0.3B)                     nrm 1
   18 Triangle Fold    skewed triangle: rising slope 1+A', falling 1-A', A' = clamp(2A-1, +-0.9); period
                       L = 2/(1+A') + 2/(1-A'); passes the origin on the rising edge; fold phase b = B L / 2
   19 Soft Fold        N = (1-b2)((1-A) tf(u) + A sin(pi/2 u)) + b2 tanh(u),  b2 = B/2  (deviation: the B
                       "outer-fold damping" is realised as a blend toward tanh, closed form)
   20 Hard Fold        N = (1-b2) tf(clamp(u, +-lim)) + b2 tanh(u), lim = 2R+1 (R = 1 + round(5A) reflections,
                       R >= 6: unlimited), b2 = B/2 (B: wall softness, same deviation as 19)
   21 Symmetric Fold   N = (1-B) |tf(u)| + B tf(u)  (B = 0: even-symmetric, octave-up; s(0) = 0 exactly)
   22 Asymmetric Fold  u >= 0: tf(u);  u < 0: T- tf(u / T-),  T- = 1 / (1 + 3B)
   23 Dynamic Fold     mode-15 fold with D_eff = D (1 + A env), env = |x| follower (attack 1 ms, release
                       5 ms * 40^B), per channel; ADAA with the current D_eff on both F1 evaluations
   24 Chebyshev Fold   N = cos(n acos(clamp(D x / 17))), n = 1 + 12a + 3B (real), Simpson-numerical ADAA
   25 Hard Clip        N = clamp(u) with C1 quadratic knee of half-width w = B (B = 0: hard)
   26 Soft Clip        N = u / (1 + |u|)
   27 Tanh             N = tanh(u),  b = 0.75 B (asymmetric bias, offset removed)
   28 Arctangent       N = (2/pi) atan(u)
   29 Tube Saturation  bias bb = 0.3A: n = (tanh(r_s u + bb) - tanh bb) / r_s, r_s = 1 (u >= 0) or 1.6 (u < 0, grid
                       conduction), sag y = n - 0.3B n^3 (static stand-in for the level-dependent sag)
   30 Diode 1          N = sgn(u) (1 - e^-|u|)
   31 Diode 2          u >= 0: 1 - e^-u;  u < 0: -c (1 - e^(0.7u)),  c = 0.6 (1 - 0.7B)
   32 Cubic            N = 1.5 (v - v^3/3), v = clamp(u)   (peak 1)
   33 Sinusoidal       N = sin(pi/2 clamp(u))
   34 Exponential      N = sgn(v) (e^(k|v|) - 1) / (e^k - 1), v = clamp(u), k = 1 + 7a + 2B
   35 Asymmetric       u >= 0: tanh(u + bb);  u < 0: tanh(r u + bb) / r  (offsets removed), r = 0.3 + 0.7 (1 - A),
                       bb = 0.35 B; peak-normalised to 0.9 (keeps the master knee, hence DC, out of play)
   36 Polynomial       N = v + c2 v^2 + c3 v^3, v = clamp(u), c3 = -a (0.2 + 0.6A), c2 = 0.5 a B
   37 Chebyshev Harm.  y = u + sum_{n=2..8} h_n T_n(clamp(u)) (the FUNDAMENTAL IS KEPT: deviation from the literal
                       spec, which has no h_1), no drive; h_n = 0.6 a w_n p_n / sum, p_n = 2A (odd n) / 2(1-A) (even),
                       w_n = (n/2)^(-1.5 + 3B)
   38 Full Wave Rect   N = |u|, D = 2^(2 (A - 0.5)) (pre-gain), b = B/2
   39 Half Wave Rect   N = u (u > 0) or B u, D as 38
   40 Soft Rect        N = sqrt(u^2 + eps^2), eps = 0.02 + 0.5A, b = B/2
   41 Bitcrush         mid-tread staircase, step 2^(1 - bits), bits = 16 * 2^(-3a) (16 -> 2), B blends back
                       0.75 B of the unquantised signal, A = TPDF dither amount (+-1 LSB at A = 1)
   42 SR Reduction     sample & hold, period 1 + 127 a^2 base samples counted at the OS rate, A = ramp
                       (0 hold .. 1 linear glide between holds), B = clock jitter
   43 Quantization     soft staircase, Nq = 64 * 2^(-5a) (64 -> 2) steps over [-1, 1], step edges tanh-rounded with
                       kappa = 0.1 + 80 (1 - A)^2, B = mid-tread (0) .. mid-rise (1)
*/

#include "DSP/Adaa.h"
#include "DSP/AmpWarp.h"
#include "DSP/FastMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace warpr::shapewarp
{
    inline constexpr int firstMode = 15;
    inline constexpr int lastMode  = 43;
    inline constexpr double kPi  = 3.14159265358979323846;
    inline constexpr double kLn2 = 0.6931471805599453;
    inline constexpr double kDriveMax = 17.0;

    //==========================================================================
    // libm-free primitives
    //==========================================================================
    inline double absd (double x) noexcept { return x < 0.0 ? -x : x; }
    inline double clampd (double x, double lo, double hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }

    inline double expNat (double x) noexcept { return fastmath::exp2 (x * 1.4426950408889634); }
    inline double lnNat (double x) noexcept { return fastmath::log2 (x) * kLn2; }          // x > 0

    /** ln cosh u, numerically stable (never overflows). */
    inline double lncosh (double u) noexcept
    {
        const double au = absd (u);
        return au + lnNat (1.0 + expNat (-2.0 * au)) - kLn2;
    }

    inline double tanhF (double u) noexcept
    {
        const double e = expNat (-2.0 * absd (u));
        const double t = (1.0 - e) / (1.0 + e);
        return u < 0.0 ? -t : t;
    }

    inline double asinhF (double z) noexcept
    {
        const double az = absd (z);
        const double r = lnNat (az + std::sqrt (az * az + 1.0));
        return z < 0.0 ? -r : r;
    }

    inline double acosF (double u) noexcept
    {
        if (u <= -1.0)
            return kPi;
        if (u >= 1.0)
            return 0.0;

        return 2.0 * fastmath::atan (std::sqrt ((1.0 - u) / (1.0 + u)));
    }

    inline double sinq (double u) noexcept { return fastmath::sin2pi (0.25 * u); }            // sin(pi/2 u)
    inline double cosq (double u) noexcept { return fastmath::sin2pi (0.25 * u + 0.25); }     // cos(pi/2 u)

    //==========================================================================
    // fold shapes (closed-form antiderivatives)
    //==========================================================================

    /** Triangle fold: identity on [-1, 1], reflects at every odd integer, period 4, odd. */
    inline double triFold (double u) noexcept
    {
        const double p = u + 1.0;
        const double r = p - 4.0 * fastmath::floorFast (0.25 * p);          // [0, 4)
        return r < 2.0 ? r - 1.0 : 3.0 - r;
    }

    /** Antiderivative of triFold, zero at u = 0, even and periodic (period 4). */
    inline double triFoldInt (double u) noexcept
    {
        const double p = u + 1.0;
        const double r = p - 4.0 * fastmath::floorFast (0.25 * p);
        return (r < 2.0 ? 0.5 * r * r - r : 3.0 * r - 0.5 * r * r - 4.0) + 0.5;
    }

    /** Antiderivative of |triFold| (period-2 tent, mean 1/2): ODD (|tf| is even), zero at 0. */
    inline double absTriInt (double u) noexcept
    {
        const double w = absd (u);
        const double n = fastmath::floorFast (0.5 * w);
        const double v = w - 2.0 * n;
        const double mag = n + (v <= 1.0 ? 0.5 * v * v : -1.0 + 2.0 * v - 0.5 * v * v);
        return u < 0.0 ? -mag : mag;
    }

    /** Skewed triangle (-1 .. +1), phase `ub`: rising slope sr over [0, w1], falling slope sf over [w1, len]. */
    inline double skewTri (double ub, double w1, double len, double invLen, double sr, double sf) noexcept
    {
        const double r = ub - len * fastmath::floorFast (ub * invLen);
        return r < w1 ? -1.0 + sr * r : 1.0 - sf * (r - w1);
    }

    /** Antiderivative of skewTri; periodic (every segment integrates to zero). */
    inline double skewTriInt (double ub, double w1, double len, double invLen, double sr, double sf) noexcept
    {
        const double r = ub - len * fastmath::floorFast (ub * invLen);
        const double t = r - w1;
        return r < w1 ? -r + 0.5 * sr * r * r : t - 0.5 * sf * t * t;
    }

    /** One stage of Multi Fold: threshold T triangle fold; F1 = T^2 TI(v / T). */
    struct FoldStageKernel
    {
        double T = 1.0, invT = 1.0;

        double f (double v) const noexcept  { return T * triFold (v * invT); }
        double F1 (double v) const noexcept { return T * T * triFoldInt (v * invT); }
    };

    //==========================================================================
    // clip shapes
    //==========================================================================

    /** clamp(u) with a C1 quadratic knee over |u| in [1 - w, 1 + w]; w = 0 is the hard clip. */
    inline double hardClip (double u, double w) noexcept
    {
        const double au = absd (u);
        double y;

        if (au <= 1.0 - w)
            y = au;
        else if (au >= 1.0 + w)
            y = 1.0;
        else
        {
            const double t = au - (1.0 - w);
            y = au - t * t / (4.0 * w);
        }

        return u < 0.0 ? -y : y;
    }

    inline double hardClipInt (double u, double w) noexcept
    {
        const double au = absd (u);

        if (au <= 1.0 - w)
            return 0.5 * au * au;

        const double k0 = 0.5 * (1.0 - w) * (1.0 - w);

        if (au < 1.0 + w)
        {
            const double t = au - (1.0 - w);
            return k0 + (1.0 - w) * t + 0.5 * t * t - t * t * t / (12.0 * w);
        }

        const double knee = k0 + 2.0 * w * (1.0 - w) + 2.0 * w * w - (2.0 / 3.0) * w * w;
        return knee + (au - (1.0 + w));
    }

    /** tanh branch pair shared by Tube (29) and Asymmetric (35):
        x0 >= 0: n = tanh(x0 + bb) - t0;  x0 < 0: n = (tanh(r x0 + bb) - t0) / r;  y = n - kB n^3. */
    inline double tubeRaw (double x0, double bb, double t0, double r, double kB) noexcept
    {
        const double rr = x0 >= 0.0 ? 1.0 : r;
        const double n = (tanhF (rr * x0 + bb) - t0) / rr;
        return n - kB * n * n * n;
    }

    inline double tubeInt (double x0, double bb, double t0, double r, double kB) noexcept
    {
        const double rr = x0 >= 0.0 ? 1.0 : r;
        const double v = rr * x0 + bb;
        const double r2 = rr * rr;

        const double lcv = lncosh (v), lcb = lncosh (bb);
        const double tv = tanhF (v);

        // integral of (tanh - t0)^3 dv  =  lncosh - T^2/2 - 3 t0 (v - T) + 3 t0^2 lncosh - t0^3 v
        auto cube = [t0] (double w, double tw, double lc)
        {
            return lc - 0.5 * tw * tw - 3.0 * t0 * (w - tw) + 3.0 * t0 * t0 * lc - t0 * t0 * t0 * w;
        };

        const double linear = (lcv - t0 * v) - (lcb - t0 * bb);
        const double cubic = cube (v, tv, lcv) - cube (bb, t0, lcb);

        return linear / r2 - kB * cubic / (r2 * r2);
    }

    /** sum_{n=2..8} h[n-2] T_n(v) (Chebyshev polynomials of the first kind, v in [-1, 1]). */
    inline double chebHarm (double v, const double* h) noexcept
    {
        double t0 = 1.0, t1 = v, y = 0.0;

        for (int n = 2; n <= 8; ++n)
        {
            const double t2 = 2.0 * v * t1 - t0;
            y += h[n - 2] * t2;
            t0 = t1;
            t1 = t2;
        }

        return y;
    }

    /** Antiderivative of chebHarm:  integral T_n = (T_{n+1} / (n+1) - T_{n-1} / (n-1)) / 2. */
    inline double chebHarmInt (double v, const double* h) noexcept
    {
        double tm = v;                           // T_{n-1}
        double tc = 2.0 * v * v - 1.0;           // T_n
        double y = 0.0;

        for (int n = 2; n <= 8; ++n)
        {
            const double tp = 2.0 * v * tc - tm; // T_{n+1}
            y += h[n - 2] * 0.5 * (tp / static_cast<double> (n + 1) - tm / static_cast<double> (n - 1));
            tm = tc;
            tc = tp;
        }

        return y;
    }

    //==========================================================================
    // The paired kernel
    //==========================================================================
    struct Kernel
    {
        int    mode = 17;
        double g = 0.0;          // blend weight lerp(x, shaped, g); exactly 0 at a = 0
        double D = 1.0;          // input drive
        double b = 0.0;          // input bias, added after the drive
        double nrm = 1.0;        // output gain
        double n0 = 0.0;         // N(b): offset removal so that s(0) = 0
        double a = 0.0, A = 0.5, B = 0.0;      // the raw knobs (stateful modes read them)
        double c[7] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };       // mode constants (see makeKernel)

        double raw (double u) const noexcept;       // N(u)
        double rawI (double u) const noexcept;      // NI(u),  NI' = N

        /** The SHAPED map s(x) (no blend with the input). */
        double f (double x) const noexcept  { return nrm * (raw (D * x + b) - n0); }
        double F1 (double x) const noexcept { return nrm * (rawI (D * x + b) / D - n0 * x); }
    };

    inline double Kernel::raw (double u) const noexcept
    {
        switch (mode)
        {
            case 15: case 16: case 23:
                return triFold (u);

            case 17:
                return (sinq (u) + c[0] * fastmath::sin2pi (0.5 * u)) * c[1];

            case 18:
                return skewTri (u + c[4], c[0], c[1], c[5], c[2], c[3]);

            case 19:
            {
                const double fold = (1.0 - c[0]) * triFold (u) + c[0] * sinq (u);
                return (1.0 - c[1]) * fold + c[1] * tanhF (u);
            }

            case 20:
            {
                const double v = clampd (u, -c[0], c[0]);
                return (1.0 - c[1]) * triFold (v) + c[1] * tanhF (u);
            }

            case 21:
            {
                const double t = triFold (u);
                return (1.0 - c[0]) * absd (t) + c[0] * t;
            }

            case 22:
                return u >= 0.0 ? triFold (u) : c[0] * triFold (u * c[1]);

            case 24:
            {
                const double v = clampd (u, -1.0, 1.0);
                return fastmath::cos2pi (c[0] * acosF (v) * (1.0 / (2.0 * kPi)));
            }

            case 25: return hardClip (u, c[0]);
            case 26: return u / (1.0 + absd (u));
            case 27: return tanhF (u);
            case 28: return (2.0 / kPi) * fastmath::atan (u);
            case 29: return tubeRaw (u, c[0], c[1], c[2], c[3]);

            case 30:
            {
                const double e = 1.0 - expNat (-absd (u));
                return u < 0.0 ? -e : e;
            }

            case 31:
                return u >= 0.0 ? 1.0 - expNat (-u) : -c[0] * (1.0 - expNat (0.7 * u));

            case 32:
            {
                const double v = clampd (u, -1.0, 1.0);
                return 1.5 * (v - v * v * v * (1.0 / 3.0));
            }

            case 33: return sinq (clampd (u, -1.0, 1.0));

            case 34:
            {
                const double v = clampd (u, -1.0, 1.0);
                const double e = (expNat (c[0] * absd (v)) - 1.0) * c[1];
                return v < 0.0 ? -e : e;
            }

            case 35: return tubeRaw (u, c[1], c[2], c[0], 0.0);

            case 36:
            {
                const double v = clampd (u, -1.0, 1.0);
                return v + c[0] * v * v + c[1] * v * v * v;
            }

            case 37:
            {
                const double v = clampd (u, -1.0, 1.0);
                return u + chebHarm (v, c);
            }

            case 38: return absd (u);
            case 39: return u > 0.0 ? u : c[0] * u;
            case 40: return std::sqrt (u * u + c[0] * c[0]);

            case 41:
            {
                const double m = fastmath::floorFast (u * c[2] + 0.5);
                return (1.0 - c[1]) * c[0] * m + c[1] * u;
            }

            case 43:
            {
                const double q = u * c[4] - c[2];
                const double kf = fastmath::floorFast (q);
                const double r = q - kf;
                const double s = 0.5 + 0.5 * tanhF (c[1] * (r - 0.5)) / c[3];
                return c[0] * (kf + s + c[2]);
            }

            default:
                return u;
        }
    }

    inline double Kernel::rawI (double u) const noexcept
    {
        switch (mode)
        {
            case 15: case 16: case 23:
                return triFoldInt (u);

            case 17:
                return (-(2.0 / kPi) * cosq (u) - c[0] * (1.0 / kPi) * fastmath::sin2pi (0.5 * u + 0.25)) * c[1];

            case 18:
                return skewTriInt (u + c[4], c[0], c[1], c[5], c[2], c[3]);

            case 19:
            {
                const double fold = (1.0 - c[0]) * triFoldInt (u) - c[0] * (2.0 / kPi) * cosq (u);
                return (1.0 - c[1]) * fold + c[1] * lncosh (u);
            }

            case 20:
            {
                const double v = clampd (u, -c[0], c[0]);
                const double fold = triFoldInt (v) + triFold (v) * (u - v);
                return (1.0 - c[1]) * fold + c[1] * lncosh (u);
            }

            case 21:
                return (1.0 - c[0]) * absTriInt (u) + c[0] * triFoldInt (u);

            case 22:
                return u >= 0.0 ? triFoldInt (u) : c[0] * c[0] * triFoldInt (u * c[1]);

            case 25: return hardClipInt (u, c[0]);

            case 26:
            {
                const double au = absd (u);
                return au - lnNat (1.0 + au);
            }

            case 27: return lncosh (u);
            case 28: return (2.0 / kPi) * (u * fastmath::atan (u) - 0.5 * lnNat (1.0 + u * u));
            case 29: return tubeInt (u, c[0], c[1], c[2], c[3]);

            case 30:
            {
                const double au = absd (u);
                return au - 1.0 + expNat (-au);
            }

            case 31:
                return u >= 0.0 ? u - 1.0 + expNat (-u)
                                : -c[0] * u + c[0] * (expNat (0.7 * u) - 1.0) / 0.7;

            case 32:
            {
                const double v = clampd (u, -1.0, 1.0);
                const double v2 = v * v;
                return 1.5 * (0.5 * v2 - v2 * v2 * (1.0 / 12.0)) + 1.5 * (v - v * v2 * (1.0 / 3.0)) * (u - v);
            }

            case 33:
            {
                const double v = clampd (u, -1.0, 1.0);
                return (2.0 / kPi) * (1.0 - cosq (v)) + sinq (v) * (u - v);
            }

            case 34:
            {
                const double v = clampd (u, -1.0, 1.0);
                const double av = absd (v);
                const double inner = ((expNat (c[0] * av) - 1.0) / c[0] - av) * c[1];
                const double e = (expNat (c[0] * av) - 1.0) * c[1];
                return inner + (v < 0.0 ? -e : e) * (u - v);
            }

            case 35: return tubeInt (u, c[1], c[2], c[0], 0.0);

            case 36:
            {
                const double v = clampd (u, -1.0, 1.0);
                const double v2 = v * v;
                return 0.5 * v2 + c[0] * v2 * v * (1.0 / 3.0) + c[1] * v2 * v2 * 0.25
                       + (v + c[0] * v2 + c[1] * v2 * v) * (u - v);
            }

            case 37:
            {
                const double v = clampd (u, -1.0, 1.0);
                return 0.5 * u * u + chebHarmInt (v, c) + chebHarm (v, c) * (u - v);
            }

            case 38: return 0.5 * u * absd (u);
            case 39: return u > 0.0 ? 0.5 * u * u : 0.5 * c[0] * u * u;
            case 40: return 0.5 * (u * std::sqrt (u * u + c[0] * c[0]) + c[0] * c[0] * asinhF (u / c[0]));

            case 41:
            {
                const double m = fastmath::floorFast (u * c[2] + 0.5);
                const double stair = m * c[0] * u - 0.5 * c[0] * c[0] * m * m;
                return (1.0 - c[1]) * stair + c[1] * 0.5 * u * u;
            }

            case 43:
            {
                const double q = u * c[4] - c[2];
                const double kf = fastmath::floorFast (q);
                const double r = q - kf;
                const double sint = 0.5 * r + (0.5 / (c[1] * c[3])) * (lncosh (c[1] * (r - 0.5)) - lncosh (0.5 * c[1]));
                return c[0] * c[0] * (0.5 * kf * kf + kf * r + sint) + c[2] * c[0] * u;
            }

            default:
                return 0.5 * u * u;          // 24 (no closed form, unused) and 42
        }
    }

    /** Kernel for `mode` (15..43) at the given knob values. */
    inline Kernel makeKernel (int mode, double a, double A, double B) noexcept
    {
        Kernel k;
        k.mode = mode;
        k.a = a;
        k.A = A;
        k.B = B;

        const double ap = a > 0.0 ? a : 0.0;
        const bool rect = mode >= 38 && mode <= 40;

        k.g = ap <= 0.0 ? 0.0 : (rect ? std::min (1.0, ap) : std::min (1.0, 20.0 * ap));
        k.D = 1.0 + ap * (1.0 + 15.0 * A);

        switch (mode)
        {
            case 15:
                k.b = 0.5 * B;
                break;

            case 16:
            {
                const int stages = 2 + static_cast<int> (3.0 * clampd (B, 0.0, 1.0) + 0.5);
                k.c[0] = static_cast<double> (stages);
                k.nrm = fastmath::exp2 (0.5 * static_cast<double> (stages - 1));
                break;
            }

            case 17:
                k.c[0] = 0.3 * B;
                k.c[1] = 1.0 / (1.0 + 0.3 * B);
                break;

            case 18:
            {
                const double ask = clampd (2.0 * A - 1.0, -0.9, 0.9);
                const double sr = 1.0 + ask, sf = 1.0 - ask;
                const double w1 = 2.0 / sr, w2 = 2.0 / sf;
                const double len = w1 + w2;
                k.c[0] = w1;
                k.c[1] = len;
                k.c[2] = sr;
                k.c[3] = sf;
                k.c[4] = 1.0 / sr;           // origin sits mid-way up the rising edge
                k.c[5] = 1.0 / len;
                k.b = 0.5 * B * len;
                break;
            }

            case 19:
                k.c[0] = clampd (A, 0.0, 1.0);
                k.c[1] = 0.5 * B;
                break;

            case 20:
            {
                const int reflections = 1 + static_cast<int> (5.0 * clampd (A, 0.0, 1.0) + 0.5);
                k.c[0] = reflections >= 6 ? 1.0e5 : static_cast<double> (2 * reflections + 1);       // 1e5: "unlimited"
                k.c[1] = 0.5 * B;
                break;
            }

            case 21:
                k.c[0] = clampd (B, 0.0, 1.0);
                break;

            case 22:
                k.c[0] = 1.0 / (1.0 + 3.0 * B);
                k.c[1] = 1.0 + 3.0 * B;
                break;

            case 24:
                k.D = k.D / kDriveMax;
                k.c[0] = clampd (1.0 + 12.0 * ap + 3.0 * B, 1.0, 16.0);
                break;

            case 25:
                k.c[0] = clampd (B, 0.0, 0.999);
                break;

            case 27:
                k.b = 0.75 * B;
                break;

            case 29:
                k.c[0] = 0.3 * A;
                k.c[1] = tanhF (k.c[0]);
                k.c[2] = 1.6;
                k.c[3] = 0.3 * B;
                break;

            case 31:
                k.c[0] = 0.6 * (1.0 - 0.7 * B);
                break;

            case 34:
            {
                const double kk = 1.0 + 7.0 * ap + 2.0 * B;
                k.c[0] = kk;
                k.c[1] = 1.0 / (expNat (kk) - 1.0);
                break;
            }

            case 35:
            {
                const double r = 0.3 + 0.7 * (1.0 - A);
                const double bb = 0.35 * B;
                const double t0 = tanhF (bb);
                const double peak = std::max (1.0 - t0, (1.0 + t0) / r);
                k.c[0] = r;
                k.c[1] = bb;
                k.c[2] = t0;
                // peak-normalised to 0.9: the output must stay under the master safety knee (0.89 / velocity gain),
                // otherwise the knee rectifies the asymmetric plateau into a DC offset AFTER the DC blockers
                k.nrm = peak > 0.9 ? 0.9 / peak : 1.0;
                break;
            }

            case 36:
                k.c[0] = 0.5 * ap * B;
                k.c[1] = -ap * (0.2 + 0.6 * A);
                break;

            case 37:
            {
                double weight[7];
                double sum = 0.0;

                for (int n = 2; n <= 8; ++n)
                {
                    const double parity = (n % 2 == 0) ? 2.0 * (1.0 - A) : 2.0 * A;
                    const double tilt = fastmath::exp2 ((-1.5 + 3.0 * B) * fastmath::log2 (0.5 * static_cast<double> (n)));
                    weight[n - 2] = parity * tilt;
                    sum += weight[n - 2];
                }

                const double scale = 0.6 * ap / std::max (sum, 1.0e-9);
                for (int n = 0; n < 7; ++n)
                    k.c[n] = weight[n] * scale;

                k.D = 1.0;
                break;
            }

            case 38: case 39:
                k.D = fastmath::exp2 (2.0 * (A - 0.5));
                if (mode == 38)
                    k.b = 0.5 * B;
                else
                    k.c[0] = clampd (B, 0.0, 1.0);
                break;

            case 40:
                k.D = 1.0;
                k.b = 0.5 * B;
                k.c[0] = 0.02 + 0.5 * A;
                break;

            case 41:
            {
                const double bits = 16.0 * fastmath::exp2 (-3.0 * ap);
                const double delta = fastmath::exp2 (1.0 - bits);
                k.D = 1.0;
                k.c[0] = delta;
                k.c[1] = 0.75 * clampd (B, 0.0, 1.0);
                k.c[2] = 1.0 / delta;
                break;
            }

            case 42:
                k.D = 1.0;
                break;

            case 43:
            {
                const double steps = 64.0 * fastmath::exp2 (-5.0 * ap);
                const double delta = 2.0 / steps;
                const double soft = 1.0 - clampd (A, 0.0, 1.0);
                const double kappa = 0.1 + 80.0 * soft * soft;
                k.D = 1.0;
                k.c[0] = delta;
                k.c[1] = kappa;
                k.c[2] = 0.5 * clampd (B, 0.0, 1.0);
                k.c[3] = tanhF (0.5 * kappa);
                k.c[4] = 1.0 / delta;
                break;
            }

            default:
                break;          // 26, 28, 30, 32, 33: no constants
        }

        k.n0 = k.raw (k.b);
        return k;
    }

    //==========================================================================
    // Per-channel state + the run step
    //==========================================================================
    struct RunInfo
    {
        int    os = 1;                 // oversampling factor of the voice
        double osRate = 96000.0;       // oscillator-rate in Hz (envelope times)
    };

    struct ChannelState
    {
        adaa::State st[5];             // ADAA histories (Multi Fold uses one per stage)
        double env = 0.0;              // Dynamic Fold follower
        double phase = 1.0;            // SR Reduction hold phase
        double hold = 0.0, prevHold = 0.0;
        std::uint32_t rng = 0x2545F491u;

        void reset() noexcept
        {
            for (auto& s : st)
                s.reset();

            env = 0.0;
            phase = 1.0;
            hold = prevHold = 0.0;
            rng = 0x2545F491u;
        }

        double rand01() noexcept            // xorshift32 -> [0, 1)
        {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return static_cast<double> (rng >> 8) * (1.0 / 16777216.0);
        }
    };

    /** The static (non-antialiased) result y = x + g (s(x) - x); the reference for the alias gates. */
    inline double naive (const Kernel& k, double x) noexcept
    {
        return x + k.g * (k.f (x) - x);
    }

    /** One oscillator-rate step of mode k.mode on value x.  Exactly x when k.g <= 0. */
    inline double run (const Kernel& k, ChannelState& cs, const RunInfo& ri, double x) noexcept
    {
        if (k.g <= 0.0)
            return x;

        double shaped;

        switch (k.mode)
        {
            case 16:
            {
                const int stages = static_cast<int> (k.c[0] + 0.5);

                // Stage 0 is ADAA'd in the x domain (remainder against x, not against the drive-scaled u): the
                // remainder form adds half the input step, which must not be multiplied by the drive.
                struct DrivenFold
                {
                    double D;
                    double f (double v) const noexcept  { return triFold (D * v); }
                    double F1 (double v) const noexcept { return triFoldInt (D * v) / D; }
                };

                const DrivenFold first { k.D };
                double v = clampd (ampwarp::runRemainder (cs.st[0], first, x), -1.0, 1.0);
                FoldStageKernel stage;

                // Every stage output is held inside its own range +-T_j: the remainder form adds half the stage's
                // input step, so on violent input steps (noise at 1x) the bound must be enforced, otherwise the
                // overshoot accumulates stage by stage and the 2^((K-1)/2) output gain amplifies it.
                for (int j = 1; j < stages; ++j)
                {
                    stage.T = fastmath::exp2 (-static_cast<double> (j));
                    stage.invT = 1.0 / stage.T;
                    v = clampd (ampwarp::runRemainder (cs.st[j], stage, v), -stage.T, stage.T);
                }

                shaped = v * k.nrm;
                break;
            }

            case 23:
            {
                const double ax = absd (x);
                const double attack = 1.0 - expNat (-1.0 / (0.001 * ri.osRate));
                const double releaseSeconds = 0.005 * fastmath::exp2 (5.321928094887362 * clampd (k.B, 0.0, 1.0));
                const double release = 1.0 - expNat (-1.0 / (releaseSeconds * ri.osRate));
                cs.env += (ax > cs.env ? attack : release) * (ax - cs.env);

                Kernel fold = k;
                fold.mode = 15;
                fold.D = k.D * (1.0 + k.A * cs.env);
                fold.b = 0.0;
                fold.n0 = 0.0;
                shaped = ampwarp::runRemainder (cs.st[0], fold, x);
                break;
            }

            case 24:
            {
                const ampwarp::RemainderKernel<Kernel> rem { k };
                shaped = x + adaa::processNumerical (cs.st[0], rem, x);
                break;
            }

            case 41:
            {
                double xin = x;

                if (k.A > 0.0)
                {
                    const double r1 = cs.rand01();
                    const double r2 = cs.rand01();
                    xin = x + k.A * k.c[0] * (r1 - r2);
                }

                shaped = ampwarp::runRemainder (cs.st[0], k, xin);
                break;
            }

            case 42:
            {
                const double period = (1.0 + 127.0 * k.a * k.a) * static_cast<double> (ri.os);
                cs.phase += 1.0 / period;

                if (cs.phase >= 1.0)
                {
                    cs.phase = std::min (0.999, (cs.phase - 1.0) + 0.5 * k.B * cs.rand01());
                    cs.prevHold = cs.hold;
                    cs.hold = x;
                }

                const double t = (1.0 - k.A) + k.A * cs.phase;
                shaped = cs.prevHold + (cs.hold - cs.prevHold) * t;
                break;
            }

            default:
                shaped = ampwarp::runRemainder (cs.st[0], k, x);
                break;
        }

        return x + k.g * (shaped - x);
    }
}
