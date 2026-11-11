#pragma once
/*
  UnisonOsc - one unison oscillator: playhead, region fold, interpolated read, pan gains.

  It runs NATIVELY at the oversampled rate (architecture decision 2): `render` produces `nOS`
  oscillator-rate samples and ACCUMULATES them into the voice's OS-rate stereo bus; the voice
  then decimates the summed bus once (OsDecimator).

  PHASE D1 WARP SLOT: the warp / shaper stages of the oscillator pipeline sit between the read
  and the accumulation (marked below).  In D1 that slot is an explicit IDENTITY pass-through -
  there is no warp DSP yet (D2 fills it).  It is documented here so the slot cannot be mistaken
  for finished processing.
*/

#include "DSP/RegionFold.h"
#include "DSP/SampleReader.h"

#include <cmath>

namespace warpr
{
    /** Stereo balance law with unity on the dominant side: pan in [-1, 1]; the other side falls
        off as cos(|pan| * pi / 2) (equal-power balance).  gl(p) == gr(-p) bit-exactly. */
    inline void balanceGains (float pan, float& gl, float& gr) noexcept
    {
        const float p = juce::jlimit (-1.0f, 1.0f, pan);
        constexpr float halfPi = 1.57079632679f;

        gl = p > 0.0f ? std::max (0.0f, std::cos (p * halfPi)) : 1.0f;     // exactly 0 at a hard pan (cos(pi/2) in float is -4e-8)
        gr = p < 0.0f ? std::max (0.0f, std::cos (-p * halfPi)) : 1.0f;
    }

    /** Position of oscillator `index` of `count` across [-1, 1] (0 for a single oscillator). */
    inline float unisonPosition (int index, int count) noexcept
    {
        return count > 1 ? 2.0f * (float) index / (float) (count - 1) - 1.0f : 0.0f;
    }

    struct UnisonOsc
    {
        double pos = 0.0;            // playhead in source frames
        int    dir = 1;              // current travel direction (+1 / -1)
        bool   ended = false;        // One-Shot ran off its end

        /** Index 0 starts exactly at the region start (or end when reversed).  Short (single-cycle
            like) regions spread the start phases of further oscillators by the golden ratio so
            unison is not phase-locked. */
        void start (const RegionFold& rg, bool reverse, int index) noexcept
        {
            ended = false;
            dir = reverse ? -1 : 1;

            double phase = 0.0;

            if (rg.mode != RegionFold::Mode::OneShot && rg.length < 4096.0 && index > 0)
            {
                const double x = (double) index * 0.6180339887498949;
                phase = x - std::floor (x);
            }

            pos = reverse ? rg.hi - phase * rg.length : rg.lo + phase * rg.length;
        }

        /** Renders `nOS` oscillator-rate samples, ADDING them (times the ramped pan gains) to busL/busR.
            rho0 / rho1 = source frames per HOST sample at the start / end of the span; the per-OS-sample
            step is rho / os, ramped linearly across the span. */
        void render (const SampleData& sd, const RegionFold& rg, int quality,
                     double rho0, double rho1, int os,
                     float gL0, float gR0, float gL1, float gR1,
                     float* busL, float* busR, int nOS) noexcept
        {
            if (ended || nOS <= 0)
                return;

            if (! rg.constrain (pos, dir))
            {
                ended = true;
                return;
            }

            const ReadParams rp = makeReadParams (sd, 0.5 * (rho0 + rho1), quality, rg.wrapTaps, rg.lo, rg.hi);

            const double invOs = 1.0 / (double) os;
            double step = rho0 * invOs;
            const double dStep = (rho1 - rho0) * invOs / (double) nOS;

            float gL = gL0, gR = gR0;
            const float dgL = (gL1 - gL0) / (float) nOS;
            const float dgR = (gR1 - gR0) / (float) nOS;

            const bool oneShot = rg.mode == RegionFold::Mode::OneShot;
            const bool seamFade = rg.mode == RegionFold::Mode::Forward && rg.xfade > 0.0;

            for (int k = 0; k < nOS; ++k)
            {
                float sL, sR;
                readFrame (rp, pos, sL, sR);

                if (seamFade)
                {
                    double t = -1.0, q = 0.0;

                    if (dir > 0)
                    {
                        if (pos > rg.hi - rg.xfade)
                        {
                            t = (pos - (rg.hi - rg.xfade)) / rg.xfade;
                            q = pos - rg.length + rg.xfade;
                        }
                    }
                    else if (pos < rg.lo + rg.xfade)
                    {
                        t = (rg.lo + rg.xfade - pos) / rg.xfade;
                        q = pos + rg.length - rg.xfade;
                    }

                    if (t >= 0.0)
                    {
                        float pL, pR;
                        readFrame (rp, q, pL, pR);

                        const float tf = (float) std::min (1.0, t);
                        constexpr float halfPi = 1.57079632679f;
                        const float cOut = std::cos (halfPi * tf), cIn = std::sin (halfPi * tf);
                        const float gOut = cOut + rg.corr * ((1.0f - tf) - cOut);
                        const float gIn  = cIn  + rg.corr * (tf - cIn);

                        sL = gOut * sL + gIn * pL;
                        sR = gOut * sR + gIn * pR;
                    }
                }
                else if (oneShot && rg.endFade > 0.0)
                {
                    const double rem = dir > 0 ? rg.hi - pos : pos - rg.lo;

                    if (rem < rg.endFade)
                    {
                        const float g = (float) std::max (0.0, rem / rg.endFade);
                        sL *= g;
                        sR *= g;
                    }
                }

                // ---- WARP / SHAPER SLOT -------------------------------------------------------------
                // D1: IDENTITY.  No warp, fold, distortion or shaper DSP exists yet; sL / sR pass through
                // unchanged.  D2 (phase / amplitude warps) and D4 (shaper) are inserted exactly here.
                // -------------------------------------------------------------------------------------

                busL[k] += sL * gL;
                busR[k] += sR * gR;
                gL += dgL;
                gR += dgR;

                if (! rg.advance (pos, dir, step))
                {
                    ended = true;
                    break;
                }

                step += dStep;
            }
        }
    };
}
