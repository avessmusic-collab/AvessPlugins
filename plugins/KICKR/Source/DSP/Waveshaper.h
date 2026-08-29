#pragma once

#include <juce_core/juce_core.h>

namespace kickr
{
    /**
        Master morphing distortion (architecture.md -> "Waveshaper / Saturator",
        plan.md Phase 2.8). Runs on the summed mono bus right after `TransientShaper`,
        still inside the OversamplingProcessor region (AD-10) — monophonic at this
        stage (Tone / Stereo / Limiter are Phase 2.9).

        Continuous morph through 7 nonlinearities in the LOCKED order
        (parameter-spec.md authoritative):
            0 tanh -> 1 cubic -> 2 asymmetric -> 3 soft-clip (quintic)
            -> 4 hard-clip -> 5 foldback -> 6 bitcrush / decimate

        Signal path (per sample):
          - pre-gain   gDrive = dbToGain(drive * 36 dB),  u = gDrive * x
          - crossfade  seg = floor(character*6) clamped 5 ; t = character*6 - seg
                       yShaped = (1-t)*curve[seg](u) + t*curve[seg+1](u)   (equal-gain / linear)
          - makeup     adaptive short-window RMS makeup (Open Q3 / AD-5):
                       msIn  one-pole of xClean*xClean   (reference = the CLEAN pre-drive
                             input — restoring to `u` blows past the +/-12 dB clamp at any
                             real drive and stops compensating)
                       msOut one-pole of yShaped*yShaped
                       target = clamp( sqrt(msIn / max(msOut, 1e-9)), 0.25, 4.0 )
                       makeup += mk_a * (target - makeup)          (~30 ms smoothing)
                       seeded from `kCurveMakeup` on reset()/first block, then held
                       frozen for one RMS window so block 1 is already close.
          - blend      out = lerp(xClean, makeup * yShaped, driveMix)
                       xClean = the PRE-drive-gain input x, so driveMix = 0 is
                       *exactly* unity / bit-transparent.

        DC from the asymmetric / foldback / bitcrush curves is NOT removed here — the
        downstream base-rate DC blocker handles it.

        RT-safe: no allocation / lock / IO. Every curve is branch-free except the
        bounded foldback loop (<= 8 iterations). msIn / msOut / makeup + the S&H hold
        state are denormal-flushed each sample; the output is sanitised.
    */
    class Waveshaper
    {
    public:
        static constexpr int   kNumCurves        = 7;
        static constexpr int   kRmsWindowSamples = 480;    // @ fsOversampled (Stage-0 Addendum)
        static constexpr float kMakeupMin        = 0.25f;
        static constexpr float kMakeupMax        = 4.0f;
        static constexpr float kMakeupSmoothMs   = 30.0f;
        static constexpr float kDriveMaxDb       = 36.0f;

        /** Static per-curve prime gains (AD-5) — the initial `makeup` value, interpolated
            at `character`, so the first block is already close. Order matches the curves:
            tanh, cubic, asym, soft-clip, hard-clip, foldback, bitcrush. Tuned by intent. */
        static constexpr float kCurveMakeup[kNumCurves] =
            { 1.0f, 1.1f, 1.2f, 1.12f, 1.25f, 1.3f, 1.15f };

        /** @param fsOversampled  the in-region sample rate (fs * osFactor). */
        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;

        /** PHASE 2.10 — OS factor changed: recompute the ~30 ms makeup smoother alpha for
            the new rate. RMS meters / makeup / S&H state are KEPT. Coefficient-only.
            (`kRmsWindowSamples` is a fixed sample count at the oversampled rate — Stage-0
            Addendum — so `rmsAlpha` is intentionally rate-independent.) */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Per-block from KickEngine (never reads APVTS itself). All three are the raw
            0..1 controls; `drive` / `character` already include any macro offset added
            upstream (Phase 2.11). */
        void setParams (float drive, float character, float driveMix) noexcept;

        float processSample (float x) noexcept;

        /** Test hook — the current makeup gain (frozen at the primed value during the
            first RMS window after a reset). */
        float getMakeup() const noexcept { return makeup; }

    private:
        float curve (int index, float u) noexcept;

        double fs { 44100.0 };

        // Coefficients.
        float rmsAlpha    { 1.0f / static_cast<float> (kRmsWindowSamples) };
        float makeupAlpha { 0.0f };

        // Per-block params.
        float gDrive   { 1.0f };
        float driveRaw { 0.3f };
        int   seg      { 0 };      // 0..5
        float segFrac  { 0.0f };   // 0..1  (0 when character == k/6 exactly)
        float mix      { 1.0f };   // driveMix

        // Running state.
        float msIn      { 0.0f };
        float msOut     { 0.0f };
        float makeup    { 1.0f };
        int   shCounter { 0 };     // curve 6 sample-and-hold counter
        float shHeld    { 0.0f };  // curve 6 held output

        bool  primed { false };
        int   warmup { 0 };        // samples left holding `makeup` at the primed value

        JUCE_LEAK_DETECTOR (Waveshaper)
    };
}
