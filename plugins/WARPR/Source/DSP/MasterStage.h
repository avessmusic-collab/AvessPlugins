#pragma once
/*
  MasterStage (architecture C14 / section J).

      voice sum -> DC blocker (one-pole HP, ~5 Hz) -> masterGain (dB, 20 ms linear slew, then to
      linear) -> safety knee soft-clip (ADAA1) -> BRICKWALL LIMITER (ceiling -0.1 dBFS) -> NaN / inf guard

  NEVER juce::dsp::Limiter (it hides a fixed 4:1 compressor first stage - house lesson).

  Limiter (owner request 2026-11-13, "a limiter with 0.1 db headroom"): stereo-linked peak limiter,
  instant attack (the gain drops on the same sample), exponential release (~100 ms to recover),
  ceiling 10^(-0.1/20).  It sits AFTER the knee: the knee keeps its soft-clip character and bounds
  pathological jumps; the limiter then guarantees the -0.1 dBFS ceiling by gain riding, never by
  clipping.  While the envelope sits at/below the ceiling the gain is exactly 1 and the samples
  pass through BIT-EXACTLY (no multiply).

  Safety knee: |x| <= 0.89 is untouched, bit-exactly; above, the output follows
      y = sgn(x) * (0.89 + 0.11 * tanh((|x| - 0.89) / 0.11))        (C1 at the knee, |y| < 1)
  The first-order antiderivative antialiasing is applied to the NONLINEAR REMAINDER only,
      y = x + (G(x) - G(x_prev)) / (x - x_prev),     G(x) = F(x) - x^2/2,  F = integral of the knee
  so that in the linear zone the result is x exactly (plain ADAA of the identity would be a
  two-point average, i.e. a low-pass) and beyond the knee the clipping is antialiased.
  G is evaluated in double precision with a stable log-cosh.
*/

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace warpr
{
    class MasterStage
    {
    public:
        static constexpr double knee  = 0.89;
        static constexpr double width = 0.11;
        static constexpr double ceiling = 0.98855309465693897;   // -0.1 dBFS = 10^(-0.1/20)

        void prepare (double newSampleRate) noexcept
        {
            dcR = 1.0 - 6.283185307179586 * 5.0 / newSampleRate;
            limRelease = std::exp (-1.0 / (0.100 * newSampleRate));   // 100 ms peak-envelope release
            gainDb.reset (newSampleRate, 0.020);
            reset();
        }

        void reset() noexcept
        {
            dcxL = dcxR = dcyL = dcyR = 0.0;
            prevL = prevR = 0.0;
            limEnv = ceiling;
            first = true;
            cachedDb = 1.0e9f;
            cachedGain = 1.0f;
        }

        /** In place on l / r.  `targetDb` = masterGain parameter (dB). */
        void process (float* l, float* r, int n, float targetDb) noexcept
        {
            if (first)
            {
                gainDb.setCurrentAndTargetValue (targetDb);
                first = false;
            }
            else
            {
                gainDb.setTargetValue (targetDb);
            }

            for (int i = 0; i < n; ++i)
            {
                const bool smoothing = gainDb.isSmoothing();
                const float db = gainDb.getNextValue();

                if (smoothing || ! juce::exactlyEqual (db, cachedDb))
                {
                    cachedGain = std::pow (10.0f, db * 0.05f);
                    cachedDb = db;
                }

                // ---- DC blocker (denormal-safe: input is sanitised first)
                const double xl = sanitise (l[i]);
                const double xr = sanitise (r[i]);

                const double yl = xl - dcxL + dcR * dcyL;
                const double yr = xr - dcxR + dcR * dcyR;
                dcxL = xl;  dcyL = yl;
                dcxR = xr;  dcyR = yr;

                // ---- gain + safety knee
                const double gl = yl * (double) cachedGain;
                const double gr = yr * (double) cachedGain;

                const double kl = softClip (gl, prevL);
                const double kr = softClip (gr, prevR);

                // ---- brickwall limiter, -0.1 dBFS: instant attack, 100 ms release, stereo-linked.
                // At/below the ceiling the envelope rests AT the ceiling, the gain is exactly 1 and
                // the knee output passes through untouched (bit-exact silence/low-level path).
                const double peak = std::max (std::abs (kl), std::abs (kr));
                limEnv = std::max (peak, ceiling + (limEnv - ceiling) * limRelease);

                if (limEnv > ceiling)
                {
                    const double g = ceiling / limEnv;
                    l[i] = (float) (kl * g);
                    r[i] = (float) (kr * g);
                }
                else
                {
                    l[i] = (float) kl;
                    r[i] = (float) kr;
                }
            }
        }

        /** The limiter's current gain-reduction factor (1 = none); for tests / metering. */
        double limiterGain() const noexcept { return limEnv > ceiling ? ceiling / limEnv : 1.0; }

        /** Antialiased knee: exposed for tests. */
        static double softClipAdaa (double x, double& previous) noexcept { return softClip (x, previous); }

    private:
        static double sanitise (float v) noexcept
        {
            if (! std::isfinite (v))
                return 0.0;

            return std::max (-1.0e6, std::min (1.0e6, (double) v));
        }

        static double knee_f (double x) noexcept
        {
            const double ax = std::abs (x);
            if (ax <= knee)
                return x;

            const double y = knee + width * std::tanh ((ax - knee) / width);
            return x < 0.0 ? -y : y;
        }

        /** G(x) = F(x) - x^2/2 for the knee function (even; 0 inside the linear zone). */
        static double remainderIntegral (double x) noexcept
        {
            const double ax = std::abs (x);
            if (ax <= knee)
                return 0.0;

            const double z = (ax - knee) / width;
            const double lnCosh = z + std::log1p (std::exp (-2.0 * z)) - 0.6931471805599453;
            return 0.5 * knee * knee + knee * (ax - knee) + width * width * lnCosh - 0.5 * ax * ax;
        }

        static double softClip (double x, double& xPrev) noexcept
        {
            double out;

            if (std::abs (x) <= knee && std::abs (xPrev) <= knee)
            {
                out = x;                                              // bit-exact below the knee
            }
            else
            {
                const double dx = x - xPrev;
                double remainder;

                if (std::abs (dx) > 1.0e-5)
                    remainder = (remainderIntegral (x) - remainderIntegral (xPrev)) / dx;
                else
                {
                    const double m = 0.5 * (x + xPrev);
                    remainder = knee_f (m) - m;
                }

                out = x + remainder;
            }

            xPrev = x;

            // Hard safety ceiling: the ADAA linearisation term ((x - xPrev)/2 of the identity part)
            // is unbounded for large inter-sample jumps, so the knee alone cannot guarantee <= 1.0.
            // The clamp only engages in that pathological regime; normal signals stay on the knee.
            return std::max (-1.0, std::min (1.0, out));
        }

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gainDb;
        double dcR = 0.9993;
        double limRelease = 0.9998, limEnv = ceiling;
        double dcxL = 0.0, dcxR = 0.0, dcyL = 0.0, dcyR = 0.0;
        double prevL = 0.0, prevR = 0.0;
        bool   first = true;
        float  cachedDb = 1.0e9f, cachedGain = 1.0f;
    };
}
