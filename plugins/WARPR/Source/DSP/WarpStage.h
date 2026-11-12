#pragma once
/*
  WarpStage - the per-oscillator warp stage plumbing (architecture "Warp state machine", phases D2, D6).

  One WarpOscState lives in every UnisonOsc.  UnisonOsc::render() calls process() once per
  oscillator-rate sample while the stage is active; the stage

    1. keeps the note-cycle clock `theta` (advanced EVERY OS sample, also while bypassed, so a warp that
       is enabled mid-note has the right cycle phase) and its parity (master cycles mod 2, Soft Sync),
    2. dispatches the selected mode through WarpModeTable (phase kernels move the read position - up to TWO taps
       and an amplitude window since D6 - amp kernels shape the value; a not-yet-implemented row is the exact
       identity, though none is left after D6),
    3. crossfades mode changes with DiscreteSwitchFade (old kernel with frozen parameters + new kernel
       with reset state, ~10 ms),
    4. applies the SeamBridge for modes whose cycle map jumps at the wrap (Mirror, and the D6 rows flagged seamJump),
    5. mixes dry / wet linearly (equal-gain: dry + mix * (wet - dry); mix >= 1 returns wet untouched),
    6. reports the SPEED of the warped read (|d P' / d n| in units of the unwarped step) so the oscillator can pick the
       mip level for the speed the read actually has (speed-aware mip selection, SampleReader.h).

  D6 additions.  Phase modes 44-53 are map modes (PhaseWarp.h evalMap); 54, 55, 58, 59 are the phase-domain
  modulation kernels and 56, 57 the amplitude-domain ones (PhaseKernel.h).  The modulator's SAMPLE source is read
  through `readMod` at the modulator playhead  P_m = lo + ratio (P_mod - lo)  (P_mod = the carrier playhead, or the
  virtual playhead the oscillator keeps while the grains drive the voice), with its own mip level.  Self FM / Cross
  Mod close their loop on the mid (L + R) / 2 of the kernel's wet output, so the read offset stays shared between the
  channels (the D2 stereo convention) and identical channels stay bit-identical.

  Bit-exactness contract (tested):  stage inactive  ==  stage active with a = 0 (every mode)  ==  any
  not-yet-implemented mode  ==  Phase Remapping with the identity curve.  In all of them the output equals the
  unwarped read.

  The read function is supplied by the oscillator: `readAt (double position, float& l, float& r)` returns
  the interpolated frame including the loop-seam / one-shot fades for that READ position.
*/

#include "DSP/Adaa.h"
#include "DSP/AmpWarp.h"
#include "DSP/DiscreteSwitchFade.h"
#include "DSP/ModOsc.h"
#include "DSP/PhaseKernel.h"
#include "DSP/PhaseWarp.h"
#include "DSP/RegionFold.h"
#include "DSP/ShapeWarp.h"
#include "DSP/WarpModeTable.h"

#include <algorithm>
#include <cmath>

namespace warpr
{
    /** Per-render-span warp description (built by the voice once per 32-sample chunk).
        a / A / B / mix / ratio ramp linearly from *0 to *1 across the span. */
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

        // D6
        int    modSource = 4;                    // modOscSource: 0 Sine, 1 Triangle, 2 Saw, 3 Square, 4 Sample
        float  ratio0 = 1.0f, ratio1 = 1.0f;     // modOscRatio (already smoothed in the log domain by the voice)
        const ShaperLutSet* curve = nullptr;     // Phase Remapping: the shaper's current curve set (nullptr = identity)
        bool   blep = true;                      // Hard Sync polyBLEP (off only for the BLEP measurement test)
        bool   grainDriven = false;              // the grains supply the value (and apply the phase modes themselves):
                                                 // the phase kernels of this stage are skipped (identity); amp modes still run
    };

    class WarpOscState
    {
    public:
        void reset (double theta0) noexcept
        {
            idle();
            theta = theta0;
            justWrapped = false;
            parity = 0;
            lastA = 0.0;
            lastAA = 0.5;
            lastB = 0.0;
            speed = 1.0;
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
            modCur.reset();
            modOld.reset();
            bridgeLeft = 0;
            lastPwValid = false;
            speed = 1.0;
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
                parity ^= 1;
            }
        }

        double getTheta() const noexcept { return theta; }

        /** Speed of the last warped read in units of the unwarped step (1 = nominal; > 1 = the read is faster and needs a
            smoother mip level).  Meaningful right after process(). */
        double speedRatio() const noexcept { return speed; }

        /** Feedback-guard activations of Self FM / Cross Mod since the note started (diagnostics / tests). */
        int guardTrips() const noexcept { return guardTotal; }

        //======================================================================
        /** One oscillator-rate sample.  `pos` / `dir` / `step`: the carrier playhead (source frames), its direction and
            its step per OS sample.  `mpos` / `mdir`: the playhead the Sample-source modulator follows (== pos, dir unless the
            grains drive the voice).  `ratioF`: modOscRatio for this sample.  `readMod (position, l, r)`: the modulator read
            (its own mip level). */
        template <class Read, class ReadDry, class ReadMod>
        void process (const WarpSpan& sp, float aF, float aA, float aB, float mix, float ratioF,
                      double pos, int dir, double step, double mpos, int mdir, const RegionFold& rg, bool stereo,
                      Read& readAt, ReadDry& readDry, ReadMod& readMod, float& outL, float& outR) noexcept
        {
            const double a = static_cast<double> (aF);
            const double A = static_cast<double> (aA);
            const double B = static_cast<double> (aB);

            if (std::abs (sp.osRate - coefRate) > 0.5)
            {
                coefs = modosc::coefsFor (sp.osRate);
                coefRate = sp.osRate;
            }

            const Frame fr { sp, pos, dir, step, mpos, mdir, static_cast<double> (ratioF), step / sp.Lc, rg, stereo };

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
                modOld = modCur;
                modCur.reset();
                bridgeLeft = 0;
            }

            const bool fading = switcher.fading();
            const WarpModeInfo& info = warpModeInfo (switcher.current());

            // The dry read is only skipped for a pure phase kernel at full wet and no crossfade.
            const bool needDry = mix < 1.0f || fading || info.kind != WarpKind::Phase || ! info.implemented;

            float dryL = 0.0f, dryR = 0.0f;
            if (needDry)
                readDry (pos, dryL, dryR);       // the DRY value is always read at the nominal speed's mip level (not the warped one's)

            float wetL = 0.0f, wetR = 0.0f;
            evalKernel (switcher.current(), a, A, B, ampCur, shapeCur, modCur, true, fr, readAt, readMod, needDry, dryL, dryR, wetL, wetR);

            if (fading)
            {
                float oldL = 0.0f, oldR = 0.0f;
                evalKernel (switcher.outgoing(), oldA, oldAA, oldB, ampOld, shapeOld, modOld, false, fr, readAt, readMod, true, dryL, dryR, oldL, oldR);

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

            if (info.kind != WarpKind::Phase || ! info.implemented)
                speed = 1.0;

            guardTotal += modCur.guardTrips + modOld.guardTrips;
            modCur.guardTrips = 0;
            modOld.guardTrips = 0;

            lastA = a;
            lastAA = A;
            lastB = B;
        }

    private:
        /** Everything about the current sample that is the same for the current and the outgoing kernel. */
        struct Frame
        {
            const WarpSpan& sp;
            double pos;
            int    dir;
            double step;
            double mpos;
            int    mdir;
            double ratio;
            double dTheta;
            const RegionFold& rg;
            bool   stereo;
        };

        /** The raw modulator read for the Sample source, shifted by `offCycles` root cycles. */
        template <class ReadMod>
        static double readModulator (const Frame& f, double offCycles, ReadMod& readMod) noexcept
        {
            const RegionFold& rg = f.rg;
            const double pm = rg.foldRead (rg.lo + f.ratio * (f.mpos - rg.lo) + static_cast<double> (f.mdir) * f.sp.Lc * offCycles);
            float l = 0.0f, r = 0.0f;
            readMod (pm, l, r);
            return 0.5 * (static_cast<double> (l) + static_cast<double> (r));
        }

        template <class Read, class ReadMod>
        void evalKernel (int mode, double a, double A, double B, adaa::State* st, shapewarp::ChannelState* shape,
                         phasewarp::ModState& ms, bool isCurrent, const Frame& f, Read& readAt, ReadMod& readMod,
                         bool dryValid, float dryL, float dryR, float& wetL, float& wetR) noexcept
        {
            const WarpSpan& sp = f.sp;
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
                phasewarp::PhaseOut po;                     // neutral until a kernel fills it

                if (sp.grainDriven)
                {
                    // fully granular: the grains already applied the phase mode to their reads, so the value entering
                    // this stage is the finished one (the read below ignores its position)
                }
                else if (info.family == WarpFamily::Mod)
                {
                    const phasewarp::ModIn mi { theta, f.dTheta, f.ratio, sp.modSource, &coefs };
                    auto rdm = [&] (double off) noexcept { return readModulator (f, off, readMod); };
                    po = phasewarp::evalMod (mode, a, A, B, mi, ms, rdm);
                }
                else
                {
                    const phasewarp::MapCtx cx { sp.curve, parity, sp.blep };
                    po = phasewarp::evalMap (mode, a, A, B, theta, f.dTheta, cx);
                }

                const bool plain = po.identity();
                const double pos = f.pos;
                double pw = pos;

                if (plain)
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
                }
                else
                {
                    const double sgn = static_cast<double> (f.dir) * sp.Lc;
                    pw = f.rg.foldRead (pos + sgn * po.d0);
                    readAt (pw, wetL, wetR);

                    if (po.g1 > 0.0f)
                    {
                        float l1 = 0.0f, r1 = 0.0f;
                        readAt (f.rg.foldRead (pos + sgn * po.d1), l1, r1);
                        wetL = po.g0 * wetL + po.g1 * l1;
                        wetR = po.g0 * wetR + po.g1 * r1;
                    }

                    if (! juce::exactlyEqual (po.amp, 1.0f))
                    {
                        wetL *= po.amp;
                        wetR *= po.amp;
                    }
                }

                if ((mode == 58 || mode == 59) && a > 0.0 && ! sp.grainDriven)
                    phasewarp::feed (mode, A, 0.5 * (static_cast<double> (wetL) + static_cast<double> (wetR)), sp.osRate, ms);

                if (isCurrent)
                {
                    // speed of the warped read relative to the unwarped step (for the mip level of the NEXT sample)
                    // (measured on the UNFOLDED offset, so loop wraps and seam crossfades of the playhead do not count)
                    const double off = plain ? 0.0 : static_cast<double> (f.dir) * sp.Lc * po.d0;
                    double q = 1.0;

                    if (lastPwValid && ! sp.grainDriven)
                    {
                        q = std::abs (static_cast<double> (f.dir) * f.step + (off - lastOff)) / std::max (f.step, 1.0e-12);

                        if (! (q < 40.0))
                            q = 1.0;                                // a reset / jump, not a speed
                    }

                    lastOff = off;
                    speed = q;

                    if (plain)
                    {
                        bridgeLeft = 0;
                    }
                    else if (info.seamJump)
                    {
                        // SeamBridge: at a cycle wrap the map may jump.  Read where continuity of the read
                        // position would have led, and split the output difference over a few samples.
                        const int total = std::max (2, sp.os / 4);

                        if (justWrapped && lastPwValid)
                        {
                            float contL = 0.0f, contR = 0.0f;
                            readAt (f.rg.foldRead (lastPw + static_cast<double> (f.dir) * lastStep), contL, contR);
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
                    lastStep = f.step;
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
                    const double g = ampwarp::flipGain (a, A, B, theta, f.dTheta);
                    wetL = static_cast<float> (xl * g);
                    wetR = static_cast<float> (xr * g);
                    break;
                }

                case 10:
                {
                    wetL = static_cast<float> (ampwarp::skewApply (xl, a, A, B, theta, f.dTheta));
                    wetR = static_cast<float> (ampwarp::skewApply (xr, a, A, B, theta, f.dTheta));
                    break;
                }

                case 56:
                case 57:
                {
                    // D6 AM / RM: the dry value times a modulator gain; a <= 0 is the exact identity
                    if (a <= 0.0)
                    {
                        wetL = dryL;
                        wetR = dryR;
                        break;
                    }

                    const phasewarp::ModIn mi { theta, f.dTheta, f.ratio, sp.modSource, &coefs };
                    auto rdm = [&] (double off) noexcept { return readModulator (f, off, readMod); };
                    const double gain = phasewarp::evalAmpGain (mode, a, A, B, mi, ms, rdm);
                    wetL = static_cast<float> (xl * gain);
                    wetR = static_cast<float> (xr * gain);
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
                        wetR = f.stereo ? static_cast<float> (shapewarp::run (kern, shape[1], ri, xr)) : wetL;
                    }
                    else
                    {
                        const auto coeffs = ampwarp::makeCoeffs (mode, a, A, B);
                        wetL = static_cast<float> (ampwarp::run (coeffs, st[0], xl));
                        wetR = f.stereo ? static_cast<float> (ampwarp::run (coeffs, st[1], xr)) : wetL;
                    }

                    break;
                }
            }
        }

        DiscreteSwitchFade switcher;
        adaa::State ampCur[2], ampOld[2];
        shapewarp::ChannelState shapeCur[2], shapeOld[2];      // D3 kernels (modes 15-43)
        phasewarp::ModState modCur, modOld;                    // D6 modulation kernels (modes 54-59)
        modosc::Coefs coefs;                                   // Sample-modulator time constants for coefRate
        double coefRate = 0.0;

        double theta = 0.0;
        bool   justWrapped = false;
        int    parity = 0;                                     // master cycles elapsed mod 2 (Soft Sync)

        double lastA = 0.0, lastAA = 0.5, lastB = 0.0;     // parameters of the previous sample
        double oldA = 0.0, oldAA = 0.5, oldB = 0.0;        // frozen parameters of the outgoing kernel

        double lastPw = 0.0, lastStep = 0.0;
        double lastOff = 0.0;                              // previous sample's unfolded read offset (speed measurement)
        bool   lastPwValid = false;
        float  bridgeOffL = 0.0f, bridgeOffR = 0.0f;
        int    bridgeLeft = 0, bridgeTotal = 2;

        double speed = 1.0;
        int    guardTotal = 0;
    };
}
