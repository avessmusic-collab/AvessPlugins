#pragma once

#include <juce_core/juce_core.h>

namespace kickr
{
    /**
        Kick-tuned attack / sustain enhancement on the SUMMED mono signal
        (architecture.md -> TransientShaper, plan.md Phase 2.3).

        Design (deliberately simple — NOT a full transient designer, no lookahead):
          - Two one-pole followers of |x| vs `fsOversampled`:
              fast : attack ~1 ms   / release ~20 ms
              slow : attack ~15 ms  / release ~150 ms
          - Transient region when `fast > slow * kThresh` (kThresh = 1.05).
          - In the transient region:  gainDb = transientAttackEff * 6
            elsewhere (sustain region): gainDb = transientSustain  * 6
          - The APPLIED gain is one-pole smoothed (~3 ms) so the region flip
            never zippers.
          - Bipolar: negative `transientAttack` softens the onset (Open Q 9).

        Position: after Tone(pre), before the master Waveshaper — inside the OS
        region (AD-10). Gain-only, so oversampling it only costs a little CPU and
        keeps the buffer plumbing to one up/down pair.

        `transientAttackEff` = `transientAttack (+ macroPunch offset)` — the macro
        offset is added upstream in KickEngine (Phase 2.11); this class just takes
        the pre-resolved value via `setParams`.

        RT-safe: no allocation / lock / IO. Follower states AND the smoothed-gain
        state are denormal-flushed every sample.
    */
    class TransientShaper
    {
    public:
        static constexpr float kThresh    = 1.05f;
        static constexpr float kMaxGainDb = 6.0f;

        /** @param fsOversampled  the in-region sample rate (fs * osFactor). */
        void prepare (double fsOversampled) noexcept;
        void reset() noexcept;

        /** 2026-09-01 — called by KickEngine on EVERY note-on: clears only the fast/slow
            envelope followers so each hit's transient detection starts from an identical
            clean state (fixes retrigger-speed-dependent attack character). Deliberately
            leaves `smoothedGain` continuous — reset() snaps it to unity, which on a note-on
            was a one-sample step of up to kMaxGainDb on a still-loud tail whenever
            Sustain/Attack were non-zero (bug-scan 2026-09-01). */
        void resetFollowers() noexcept;

        /** PHASE 2.10 — OS factor changed: recompute the five one-pole time-constants for
            the new rate. Follower + smoothed-gain state are KEPT. Coefficient-only. */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Per-block (called from KickEngine — never reads APVTS itself).
            Both arguments are the bipolar −1…+1 controls; `attackBipolar` already
            includes any macro offset. */
        void setParams (float attackBipolar, float sustainBipolar) noexcept;

        float processSample (float x) noexcept;

        /** PHASE 2.9 — stereo-linked: the dual follower + gain run on the mono
            detector `0.5*(l + r)` and the SAME smoothed gain is applied to both
            channels, so the transient stays phase-coherent across L/R. */
        void processStereo (float& l, float& r) noexcept;

    private:
        /** Advance the followers + smoothed gain for one detector sample and return
            the applied gain. Shared by processSample / processStereo. */
        float computeGain (float detector) noexcept;

        double fs { 44100.0 };

        // One-pole coefficients (set in prepare()).
        float fastAtk { 0.0f };
        float fastRel { 0.0f };
        float slowAtk { 0.0f };
        float slowRel { 0.0f };
        float gainCoef { 0.0f };

        // Running state.
        float fastEnv      { 0.0f };
        float slowEnv      { 0.0f };
        float smoothedGain { 1.0f };

        // Per-block params.
        float attackAmt  { 0.0f };
        float sustainAmt { 0.0f };
    };
}
