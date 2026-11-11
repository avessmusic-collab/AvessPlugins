#pragma once
/*
  RegionFold - maps the playhead into the sample region [start, end) (architecture C2).

    Forward Loop : wrap with an automatic SEAM CROSSFADE.  Seam policy (the spec has no loop-fade
                   parameter, so it is internal):
                     X = 0 when the data is periodic (the full single-cycle wave) or the seam is clean
                         (value jump equals the mean neighbouring slope, slope mismatch small);
                     else X = min(L/4, 20 ms): during the last X frames the output is a blend of
                         s(P) and s(P - L + X); the playhead then wraps to lo + X.
                   The blend law is power-complementary for uncorrelated material and equal-gain
                   for correlated material (correlation estimated from 16 points when the region
                   changes), so periodic content is not amplitude-modulated by the fade.
    Ping-Pong    : reflect at both ends (value-continuous, slope flips).
    One-Shot     : play once; a 5 ms fade into the end, then silence (`advance` returns false).
                   (5 ms keeps the per-sample step of a full-scale fade under the 0.01 seam bound.)

  `configure` is O(1) (O(16) when the seam is not clean) and allocation-free.
*/

#include "DSP/SampleData.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    struct RegionFold
    {
        enum class Mode { Forward, PingPong, OneShot };

        Mode   mode = Mode::Forward;
        double lo = 0.0, hi = 1.0, length = 1.0;      // frames
        double xfade = 0.0;                           // seam crossfade length X (frames); 0 = none
        float  corr = 1.0f;                           // 0 (uncorrelated) .. 1 (correlated) crossfade law
        bool   wrapTaps = false;                      // interpolation taps wrap around the loop (X == 0, Forward)
        double endFade = 0.0;                         // One-Shot fade-out length (frames)

        //----------------------------------------------------------------------
        void configure (const SampleData& sd, float startNorm, float endNorm, Mode newMode) noexcept
        {
            const double n = (double) sd.numFrames;
            const double minLen = std::min (n, std::max (64.0, 0.002 * sd.fileSampleRate));

            double l = (double) juce::jlimit (0.0f, 1.0f, startNorm) * n;
            double h = (double) juce::jlimit (0.0f, 1.0f, endNorm) * n;

            if (h < l + minLen)
            {
                h = l + minLen;
                if (h > n)
                {
                    h = n;
                    l = n - minLen;
                }
            }

            if (cachedFor == &sd && juce::exactlyEqual (l, lo) && juce::exactlyEqual (h, hi) && newMode == mode)
                return;

            cachedFor = &sd;
            lo = l;
            hi = h;
            length = h - l;
            mode = newMode;
            xfade = 0.0;
            corr = 1.0f;
            wrapTaps = false;
            endFade = 0.0;

            if (mode == Mode::Forward)
            {
                const bool periodicFull = sd.singleCycle && lo <= 0.0 && hi >= n;

                if (length >= 16.0 && ! periodicFull && ! seamIsClean (sd))
                {
                    xfade = std::min (length * 0.25, 0.02 * sd.fileSampleRate);
                    corr = estimateCorrelation (sd);
                }

                wrapTaps = xfade <= 0.0;
            }
            else if (mode == Mode::OneShot)
            {
                endFade = std::min (length * 0.25, std::max (1.0, 0.005 * sd.fileSampleRate));
            }
        }

        //----------------------------------------------------------------------
        /** Re-validates the playhead after the region moved.  Returns false when a One-Shot has ended. */
        bool constrain (double& pos, int& dir) const noexcept
        {
            if (pos >= lo && pos <= hi)
                return true;

            switch (mode)
            {
                case Mode::Forward:
                {
                    double rel = std::fmod (pos - lo, length);
                    if (rel < 0.0)
                        rel += length;
                    pos = lo + rel;
                    return true;
                }

                case Mode::PingPong:
                    pos = std::clamp (pos, lo, hi);
                    return true;

                case Mode::OneShot:
                    if (dir > 0)
                    {
                        if (pos >= hi)
                            return false;
                        pos = lo;
                    }
                    else
                    {
                        if (pos <= lo)
                            return false;
                        pos = hi;
                    }
                    return true;
            }

            return true;
        }

        /** Moves the playhead by `step` frames in direction `dir` and folds it into the region.
            Returns false when a One-Shot ran past its end. */
        bool advance (double& pos, int& dir, double step) const noexcept
        {
            pos += (double) dir * step;

            switch (mode)
            {
                case Mode::Forward:
                {
                    const double period = length - xfade;

                    if (dir > 0)
                    {
                        if (pos >= hi)
                        {
                            double ov = pos - hi;
                            if (ov >= period)
                                ov = std::fmod (ov, period);
                            pos = lo + xfade + ov;
                        }
                    }
                    else if (pos < lo)
                    {
                        double ov = lo - pos;
                        if (ov >= period)
                            ov = std::fmod (ov, period);
                        pos = hi - xfade - ov;
                    }

                    return true;
                }

                case Mode::PingPong:
                    for (int guard = 0; guard < 8; ++guard)
                    {
                        if (pos >= hi)
                        {
                            pos = 2.0 * hi - pos;
                            dir = -1;
                        }
                        else if (pos <= lo)
                        {
                            pos = 2.0 * lo - pos;
                            dir = 1;
                        }
                        else
                        {
                            break;
                        }
                    }

                    pos = std::clamp (pos, lo, hi);
                    return true;

                case Mode::OneShot:
                    return dir > 0 ? pos < hi : pos > lo;
            }

            return true;
        }

        /** Folds an arbitrary READ position (playhead + phase-warp offset) back into the region without
            touching any playhead state: Forward = wrap, Ping-Pong = reflect, One-Shot = clamp.
            A position already inside the region is returned unchanged (bit-exact). */
        double foldRead (double p) const noexcept
        {
            switch (mode)
            {
                case Mode::Forward:
                {
                    if (p >= lo && p < hi)
                        return p;

                    double rel = std::fmod (p - lo, length);
                    if (rel < 0.0)
                        rel += length;
                    return lo + rel;
                }

                case Mode::PingPong:
                {
                    if (p >= lo && p <= hi)
                        return p;

                    const double period = 2.0 * length;
                    double rel = std::fmod (p - lo, period);
                    if (rel < 0.0)
                        rel += period;
                    if (rel > length)
                        rel = period - rel;
                    return lo + rel;
                }

                case Mode::OneShot:
                    return std::clamp (p, lo, hi);
            }

            return p;
        }

    private:
        const SampleData* cachedFor = nullptr;

        /** True when looping [lo, hi) needs no fade: the jump across the seam equals the mean of the
            neighbouring slopes and the two slopes agree. */
        bool seamIsClean (const SampleData& sd) const noexcept
        {
            const int hiI = std::clamp ((int) std::floor (hi), 2, sd.numFrames);
            const int loI = std::clamp ((int) std::floor (lo), 0, std::max (0, sd.numFrames - 2));

            for (int c = 0; c < sd.numChannels; ++c)
            {
                const float* d = sd.levels[0].channel (c);
                const float a1 = d[hiI - 1], a0 = d[hiI - 2], b0 = d[loI], b1 = d[loI + 1];
                const float jump = b0 - a1;
                const float slopeA = a1 - a0, slopeB = b1 - b0;

                if (std::abs (jump - 0.5f * (slopeA + slopeB)) >= 0.002f || std::abs (slopeA - slopeB) >= 0.002f)
                    return false;
            }

            return true;
        }

        float estimateCorrelation (const SampleData& sd) const noexcept
        {
            const float* d = sd.levels[0].channel (0);
            const int last = sd.numFrames - 1;
            double saa = 0.0, sbb = 0.0, sab = 0.0;

            for (int j = 0; j < 16; ++j)
            {
                const double t = ((double) j + 0.5) / 16.0;
                const double p = hi - xfade + t * xfade;
                const double q = p - length + xfade;
                const double a = d[std::clamp ((int) p, 0, last)];
                const double b = d[std::clamp ((int) q, 0, last)];

                saa += a * a;
                sbb += b * b;
                sab += a * b;
            }

            const double den = std::sqrt (saa * sbb) + 1.0e-20;
            return (float) std::clamp (sab / den, 0.0, 1.0);
        }
    };
}
