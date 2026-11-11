#pragma once
/*
  WarpStage - the per-oscillator warp stage plumbing (architecture "Warp state machine", phase D2).

  One WarpOscState lives in every UnisonOsc.  UnisonOsc::render() calls process() once per
  oscillator-rate sample while the stage is active; the stage

    1. keeps the note-cycle clock `theta` (advanced EVERY OS sample, also while bypassed, so a warp that
       is enabled mid-note has the right cycle phase),
    2. dispatches the selected mode through WarpModeTable (phase kernels move the read position, amp
       kernels shape the value; not-yet-implemented modes are the exact identity),
    3. crossfades mode changes with DiscreteSwitchFade (old kernel with frozen parameters + new kernel
       with reset state, ~10 ms),
    4. applies the SeamBridge for modes whose cycle map jumps at the wrap (Mirror),
    5. mixes dry / wet linearly (equal-gain: dry + mix * (wet - dry); mix >= 1 returns wet untouched).

  Bit-exactness contract (tested):  stage inactive  ==  stage active with a = 0 (every implemented
  mode)  ==  any not-yet-implemented mode.  In all three the output equals the unwarped read.

  The read function is supplied by the oscillator: `readAt (double position, float& l, float& r)` returns
  the interpolated frame including the loop-seam / one-shot fades for that READ position.
*/

#include "DSP/Adaa.h"
#include "DSP/AmpWarp.h"
#include "DSP/DiscreteSwitchFade.h"
#include "DSP/PhaseWarp.h"
#include "DSP/RegionFold.h"
#include "DSP/ShapeWarp.h"
#include "DSP/WarpModeTable.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    /** Per-render-span warp description (built by the voice once per 32-sample chunk).
        a / A / B / mix ramp linearly from *0 to *1 across the span. */
    struct WarpSpan
    {
        bool   active = false;       // false: stage bypassed (bit-exact D1 path); theta still advances
        int    mode = 17;            // warpMode index 0..59
        float  a0 = 0.0f,  a1 = 0.0f;
        float  A0 = 0.5f,  A1 = 0.5f;
        float  B0 = 0.0f,  B1 = 0.0f;
        float  mix0 = 1.0f, mix1 = 1.0f;     // warpMix * enable ramp
        double Lc = 1.0;             // source frames per root-note cycle (fileSampleRate / midiToHz(rootNote))
        int    fadeLen = 480;        // mode-switch crossfade length in OS samples (~10 ms)
        int    os = 1;               // oversampling factor of the voice
        double osRate = 96000.0;     // oscillator-rate in Hz (os * host rate): envelope / hold times of D3 modes
    };

    class WarpOscState
    {
    public:
        void reset (double theta0) noexcept
        {
            idle();
            theta = theta0;
            justWrapped = false;
            lastA = 0.0;
            lastAA = 0.5;
            lastB = 0.0;
        }

        /** The stage is bypassed this span: forget kernel state (a re-enable starts clean). */
        void idle() noexcept
        {
            switcher.reset();
            ampCur[0].reset();
            ampCur[1].reset();
            ampOld[0].reset();
            ampOld[1].reset();
            shapeCur[0].reset();
            shapeCur[1].reset();
            shapeOld[0].reset();
            shapeOld[1].reset();
            bridgeLeft = 0;
            lastPwValid = false;
        }

        /** Cycle clock: call once per OS sample with step / Lc (source frames per OS sample / frames per cycle). */
        void advanceTheta (double dTheta) noexcept
        {
            theta += dTheta;
            justWrapped = false;

            if (theta >= 1.0)
            {
                theta -= std::floor (theta);
                justWrapped = true;
            }
        }

        double getTheta() const noexcept { return theta; }

        //======================================================================
        template <class Read>
        void process (const WarpSpan& sp, float aF, float aA, float aB, float mix,
                      double pos, int dir, double step, const RegionFold& rg, bool stereo,
                      Read& readAt, float& outL, float& outR) noexcept
        {
            const double a = static_cast<double> (aF);
            const double A = static_cast<double> (aA);
            const double B = static_cast<double> (aB);
            const double dTheta = step / sp.Lc;

            if (switcher.request (sp.mode, sp.fadeLen))
            {
                // outgoing kernel keeps the parameters (and ADAA history) it had at the switch
                oldA = lastA;
                oldAA = lastAA;
                oldB = lastB;
                ampOld[0] = ampCur[0];
                ampOld[1] = ampCur[1];
                ampCur[0].reset();
                ampCur[1].reset();
                shapeOld[0] = shapeCur[0];
                shapeOld[1] = shapeCur[1];
                shapeCur[0].reset();
                shapeCur[1].reset();
                bridgeLeft = 0;
            }

            const bool fading = switcher.fading();
            const WarpModeInfo& info = warpModeInfo (switcher.current());

            // The dry read is only skipped for a pure phase kernel at full wet and no crossfade.
            const bool needDry = mix < 1.0f || fading || info.kind != WarpKind::Phase || ! info.implemented;

            float dryL = 0.0f, dryR = 0.0f;
            if (needDry)
                readAt (pos, dryL, dryR);

            float wetL = 0.0f, wetR = 0.0f;
            evalKernel (switcher.current(), a, A, B, ampCur, shapeCur, true, sp, pos, dir, step, rg, stereo, dTheta,
                        readAt, needDry, dryL, dryR, wetL, wetR);

            if (fading)
            {
                float oldL = 0.0f, oldR = 0.0f;
                evalKernel (switcher.outgoing(), oldA, oldAA, oldB, ampOld, shapeOld, false, sp, pos, dir, step, rg, stereo, dTheta,
                            readAt, true, dryL, dryR, oldL, oldR);

                const float t = switcher.advance();
                wetL = oldL + t * (wetL - oldL);
                wetR = oldR + t * (wetR - oldR);
            }

            if (mix >= 1.0f)
            {
                outL = wetL;
                outR = wetR;
            }
            else
            {
                outL = dryL + mix * (wetL - dryL);
                outR = dryR + mix * (wetR - dryR);
            }

            lastA = a;
            lastAA = A;
            lastB = B;
        }

    private:
        template <class Read>
        void evalKernel (int mode, double a, double A, double B, adaa::State* st, shapewarp::ChannelState* shape, bool isCurrent,
                         const WarpSpan& sp, double pos, int dir, double step, const RegionFold& rg, bool stereo,
                         double dTheta, Read& readAt, bool dryValid, float dryL, float dryR,
                         float& wetL, float& wetR) noexcept
        {
            const WarpModeInfo& info = warpModeInfo (mode);

            if (! info.implemented)
            {
                // STATIC MAPPING of not-yet-implemented modes: identity (see WarpModeTable.h)
                wetL = dryL;
                wetR = dryR;
                return;
            }

            if (info.kind == WarpKind::Phase)
            {
                const double d = phasewarp::delta (mode, a, A, B, theta, dTheta);

                if (juce::exactlyEqual (d, 0.0))
                {
                    if (dryValid)
                    {
                        wetL = dryL;
                        wetR = dryR;
                    }
                    else
                    {
                        readAt (pos, wetL, wetR);
                    }

                    if (isCurrent)
                    {
                        bridgeLeft = 0;
                        lastPw = pos;
                        lastStep = step;
                        lastPwValid = true;
                    }

                    return;
                }

                const double pw = rg.foldRead (pos + static_cast<double> (dir) * sp.Lc * d);
                readAt (pw, wetL, wetR);

                if (isCurrent)
                {
                    if (info.seamJump)
                    {
                        // SeamBridge: at a cycle wrap the map may jump.  Read where continuity of the read
                        // position would have led, and split the output difference over a few samples.
                        const int total = std::max (2, sp.os / 4);

                        if (justWrapped && lastPwValid)
                        {
                            float contL = 0.0f, contR = 0.0f;
                            readAt (rg.foldRead (lastPw + static_cast<double> (dir) * lastStep), contL, contR);
                            bridgeOffL = contL - wetL;
                            bridgeOffR = contR - wetR;
                            bridgeTotal = total;
                            bridgeLeft = total - 1;
                        }

                        if (bridgeLeft > 0)
                        {
                            const float w = static_cast<float> (bridgeLeft) / static_cast<float> (bridgeTotal);
                            wetL += bridgeOffL * w;
                            wetR += bridgeOffR * w;
                            --bridgeLeft;
                        }
                    }

                    lastPw = pw;
                    lastStep = step;
                    lastPwValid = true;
                }

                return;
            }

            // ---- amplitude domain (needs the dry value) ----
            const double xl = static_cast<double> (dryL);
            const double xr = static_cast<double> (dryR);

            switch (mode)
            {
                case 8:
                {
                    const double g = ampwarp::flipGain (a, A, B, theta, dTheta);
                    wetL = static_cast<float> (xl * g);
                    wetR = static_cast<float> (xr * g);
                    break;
                }

                case 10:
                {
                    wetL = static_cast<float> (ampwarp::skewApply (xl, a, A, B, theta, dTheta));
                    wetR = static_cast<float> (ampwarp::skewApply (xr, a, A, B, theta, dTheta));
                    break;
                }

                default:
                {
                    if (mode >= shapewarp::firstMode && mode <= shapewarp::lastMode)
                    {
                        // D3: folds / dynamics / distortions / rectifiers / quantisers (ShapeWarp.h).
                        // a <= 0 is the exact identity (shapewarp::run returns x too): skip the kernel build.
                        if (a <= 0.0)
                        {
                            wetL = dryL;
                            wetR = dryR;
                            break;
                        }

                        const auto kern = shapewarp::makeKernel (mode, a, A, B);
                        const shapewarp::RunInfo ri { sp.os, sp.osRate };
                        wetL = static_cast<float> (shapewarp::run (kern, shape[0], ri, xl));
                        wetR = stereo ? static_cast<float> (shapewarp::run (kern, shape[1], ri, xr)) : wetL;
                    }
                    else
                    {
                        const auto coeffs = ampwarp::makeCoeffs (mode, a, A, B);
                        wetL = static_cast<float> (ampwarp::run (coeffs, st[0], xl));
                        wetR = stereo ? static_cast<float> (ampwarp::run (coeffs, st[1], xr)) : wetL;
                    }

                    break;
                }
            }
        }

        DiscreteSwitchFade switcher;
        adaa::State ampCur[2], ampOld[2];
        shapewarp::ChannelState shapeCur[2], shapeOld[2];      // D3 kernels (modes 15-43)

        double theta = 0.0;
        bool   justWrapped = false;

        double lastA = 0.0, lastAA = 0.5, lastB = 0.0;     // parameters of the previous sample
        double oldA = 0.0, oldAA = 0.5, oldB = 0.0;        // frozen parameters of the outgoing kernel

        double lastPw = 0.0, lastStep = 0.0;
        bool   lastPwValid = false;
        float  bridgeOffL = 0.0f, bridgeOffR = 0.0f;
        int    bridgeLeft = 0, bridgeTotal = 2;
    };
}
