#pragma once
/*
  UnisonOsc - one unison oscillator: playhead, region fold, interpolated read, warp stage, pan gains.

  It runs NATIVELY at the oversampled rate (architecture decision 2): `render` produces `nOS`
  oscillator-rate samples and ACCUMULATES them into the voice's OS-rate stereo bus; the voice
  then decimates the summed bus once (OsDecimator).

  WARP SLOT (phase D2): between the read and the accumulation sits the WarpOscState stage
  (WarpStage.h).  Passing `warp == nullptr` (or a span with active == false) is the bit-exact D1 path.
  SHAPER SLOT (phase D4): right after the warp stage (ShaperEngine.h, ShaperOscState).  A null or inactive
  ShaperSpan leaves the value untouched (bit-exact); the ADAA history is dropped while inactive.
  GRANULAR SOURCE (phase D7a): a GrainSpan (GrainEngine.h) replaces the Classic read as the value that enters the
  warp stage.  The grain engine is rendered first for the whole span (at min(OS, 2) x the host rate, up-sampled to
  the OS rate), then every OS sample takes its value from that buffer:  weight 1 = pure granular (the Classic read
  is skipped and the Classic playhead holds), 0 < weight < 1 = playMode crossfade (both run, linear blend),
  inactive / nullptr = the bit-exact Classic path.  Phase-domain warps ask `readAt` for a shifted position; in
  granular mode there is no single playhead to shift, so the oscillator-level stage sees the grain value (an identity
  for phase modes) and the GRAIN ENGINE applies the phase mode itself, per grain head (D7b; GrainSpan::warp is filled
  by the voice).  Amplitude-domain modes are applied here, on the grain sum, exactly as for the Classic read.

  The interpolated read is wrapped in `readAtDir (position, direction)` which also applies the forward-loop seam
  crossfade / one-shot end fade for THAT read position, so phase warps (which read at P') and grains (which
  travel in their own direction) get the same seam treatment as the plain playhead.
*/

#include "DSP/GrainEngine.h"
#include "DSP/RegionFold.h"
#include "DSP/SampleReader.h"
#include "DSP/ShaperEngine.h"
#include "DSP/WarpStage.h"

#include <cmath>
#include <cstdint>

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
        WarpOscState warp;           // note-cycle clock + warp kernels (D2)
        ShaperOscState shaper;       // transfer-curve shaper ADAA history (D4)
        GrainEngine grain;           // granular source (D7a)

        /** Index 0 starts exactly at the region start (or end when reversed).  Short (single-cycle
            like) regions spread the start phases of further oscillators by the golden ratio so
            unison is not phase-locked.  `Lc` (source frames per root-note cycle, 0 = unknown) seeds
            the warp cycle clock: theta0 = frac((pos - lo) / Lc).  `grainSeed` seeds the grain PRNG. */
        void start (const RegionFold& rg, bool reverse, int index, double Lc = 0.0, std::uint32_t grainSeed = 1u) noexcept
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

            double theta0 = 0.0;
            if (Lc > 0.0)
            {
                const double c = (pos - rg.lo) / Lc;
                theta0 = c - std::floor (c);
            }
            warp.reset (theta0);
            shaper.idle();
            grain.start (grainSeed);
        }

        /** Renders `nOS` oscillator-rate samples, ADDING them (times the ramped pan gains) to busL/busR.
            rho0 / rho1 = source frames per HOST sample at the start / end of the span; the per-OS-sample
            step is rho / os, ramped linearly across the span.  `warpSpan` (optional) describes the warp
            stage for this span; nullptr or active == false leaves the read untouched.  `shaperSpan` (optional)
            describes the transfer-curve shaper stage that follows the warp stage (same null / inactive rule).
            `grainSpan` (optional) describes the granular source (null / inactive = Classic only). */
        void render (const SampleData& sd, const RegionFold& rg, int quality,
                     double rho0, double rho1, int os,
                     float gL0, float gR0, float gL1, float gR1,
                     float* busL, float* busR, int nOS,
                     const WarpSpan* warpSpan = nullptr,
                     const ShaperSpan* shaperSpan = nullptr,
                     const GrainSpan* grainSpan = nullptr) noexcept
        {
            if (nOS <= 0)
                return;

            const bool grainPresent = grainSpan != nullptr;
            const bool grainActive = grainPresent && grainSpan->active && grainSpan->scratch != nullptr;
            const bool grainFull = grainActive && grainSpan->w0 >= 1.0f && grainSpan->w1 >= 1.0f;

            if (grainPresent && ! grainActive)
                grain.idle();

            if (ended && ! grainActive)
                return;

            // Fully granular: the Classic playhead holds (it is not read), so it is not re-validated either.
            if (! ended && ! grainFull && ! rg.constrain (pos, dir))
            {
                ended = true;

                if (! grainActive)
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

            // Interpolated read at an arbitrary READ position travelling in direction `d`, with the seam / end
            // fades for that position.
            auto readAtDir = [&] (double p, int d, float& sL, float& sR) noexcept
            {
                readFrame (rp, p, sL, sR);

                if (seamFade)
                {
                    double t = -1.0, q = 0.0;

                    if (d > 0)
                    {
                        if (p > rg.hi - rg.xfade)
                        {
                            t = (p - (rg.hi - rg.xfade)) / rg.xfade;
                            q = p - rg.length + rg.xfade;
                        }
                    }
                    else if (p < rg.lo + rg.xfade)
                    {
                        t = (rg.lo + rg.xfade - p) / rg.xfade;
                        q = p + rg.length - rg.xfade;
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
                    const double rem = d > 0 ? rg.hi - p : p - rg.lo;

                    if (rem < rg.endFade)
                    {
                        const float g = (float) std::max (0.0, rem / rg.endFade);
                        sL *= g;
                        sR *= g;
                    }
                }
            };

            auto readAt = [&] (double p, float& sL, float& sR) noexcept
            {
                readAtDir (p, dir, sL, sR);
            };

            // ---- granular source setup: the whole span is rendered up front ----------------------------
            GrainOut grainOut;
            float gw = 0.0f, dgw = 0.0f;

            if (grainActive)
            {
                grainOut = grain.render (*grainSpan, rg, sd, 0.5 * (rho0 + rho1), os, nOS, readAtDir);
                gw = grainSpan->w0;
                dgw = (grainSpan->w1 - grainSpan->w0) / (float) nOS;
            }

            float gSL = 0.0f, gSR = 0.0f, curGw = 0.0f;

            // The value entering the warp stage: the Classic read, the grain value, or the playMode crossfade.
            auto readSrc = [&] (double p, float& sL, float& sR) noexcept
            {
                if (! grainActive)
                {
                    readAt (p, sL, sR);
                    return;
                }

                if (curGw >= 1.0f)
                {
                    sL = gSL;
                    sR = gSR;
                    return;
                }

                if (ended)                                            // the Classic part ran off its end: silent
                {
                    sL = curGw * gSL;
                    sR = curGw * gSR;
                    return;
                }

                readAt (p, sL, sR);
                sL += curGw * (gSL - sL);
                sR += curGw * (gSR - sR);
            };

            // ---- warp stage setup ------------------------------------------------------------------
            const bool warpPresent = warpSpan != nullptr;
            const bool warpActive = warpPresent && warpSpan->active;
            const double invLc = warpPresent && warpSpan->Lc > 0.0 ? 1.0 / warpSpan->Lc : 0.0;
            const bool stereoSrc = rp.stereo;

            if (warpPresent && ! warpActive)
                warp.idle();

            float wa = 0.0f, wA = 0.5f, wB = 0.0f, wm = 1.0f;
            float dwa = 0.0f, dwA = 0.0f, dwB = 0.0f, dwm = 0.0f;

            if (warpActive)
            {
                const float inv = 1.0f / (float) nOS;
                wa = warpSpan->a0;   dwa = (warpSpan->a1 - warpSpan->a0) * inv;
                wA = warpSpan->A0;   dwA = (warpSpan->A1 - warpSpan->A0) * inv;
                wB = warpSpan->B0;   dwB = (warpSpan->B1 - warpSpan->B0) * inv;
                wm = warpSpan->mix0; dwm = (warpSpan->mix1 - warpSpan->mix0) * inv;
            }

            // ---- shaper stage setup ----------------------------------------------------------------
            const bool shaperPresent = shaperSpan != nullptr;
            const bool shaperActive = shaperPresent && shaperSpan->active && shaperSpan->cur != nullptr;

            if (shaperPresent && ! shaperActive)
                shaper.idle();

            float shMorph = 0.0f, shMix = 1.0f, shW = 1.0f;
            float dshMorph = 0.0f, dshMix = 0.0f, dshW = 0.0f;

            if (shaperActive)
            {
                const float inv = 1.0f / (float) nOS;
                shMorph = shaperSpan->morph0;  dshMorph = (shaperSpan->morph1 - shaperSpan->morph0) * inv;
                shMix   = shaperSpan->mix0;    dshMix   = (shaperSpan->mix1 - shaperSpan->mix0) * inv;
                shW     = shaperSpan->w0;      dshW     = (shaperSpan->w1 - shaperSpan->w0) * inv;
            }

            for (int k = 0; k < nOS; ++k)
            {
                float sL, sR;

                if (grainActive)
                {
                    gSL = grainOut.l[k];
                    gSR = grainOut.r[k];
                    curGw = gw;
                    gw += dgw;
                }

                if (warpActive)
                {
                    warp.process (*warpSpan, wa, wA, wB, wm, pos, dir, step, rg, stereoSrc, readSrc, sL, sR);
                    wa += dwa;
                    wA += dwA;
                    wB += dwB;
                    wm += dwm;
                }
                else
                {
                    readSrc (pos, sL, sR);
                }

                // ---- SHAPER SLOT (D4): after the warp stage, before pan / accumulation ---------------------
                if (shaperActive)
                {
                    shaper.process (*shaperSpan, (double) shMorph, (double) shMix, (double) shW, sL, sR, stereoSrc);
                    shMorph += dshMorph;
                    shMix += dshMix;
                    shW += dshW;
                }

                busL[k] += sL * gL;
                busR[k] += sR * gR;
                gL += dgL;
                gR += dgR;

                if (warpPresent)
                    warp.advanceTheta (step * invLc);

                if (! grainFull && ! ended && ! rg.advance (pos, dir, step))
                {
                    ended = true;

                    if (! grainActive)
                        break;
                }

                step += dStep;
            }
        }
    };
}
