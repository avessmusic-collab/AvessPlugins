#pragma once

#include <juce_core/juce_core.h>

namespace kickr
{
    /**
        Body-layer amplitude envelope: AD, sustain 0 (architecture.md -> AmplitudeEnvelope).

        - Attack: fixed-fast `kBodyAttackMs = 2.0` ms raised-cosine ramp (click-free).
          `bodyAttack` is deliberately NOT a parameter (Open Q 9).
        - Decay: one-pole exponential, `coef = expDecayCoef(bodyDecay_ms, fs)`
          (~-60 dB over `bodyDecay`).
        - `isActive()` is false once the tail is below -90 dB AND past a short minimum
          length — feeds KickVoice::isActive(). PHASE 2.3: a hard `maxLength` fallback
          (~2x decay + 50 ms, always past the -90 dB point of a one-pole decay) guarantees
          the voice frees under machine-gun retriggering even if the exp tail never gets
          denormal-flushed to exactly zero.

        Ticks at `fsOversampled` (AD-10). All state denormal-flushed.
    */
    class AmplitudeEnvelope
    {
    public:
        static constexpr float kBodyAttackMs = 2.0f;
        static constexpr float kFloorDb      = -90.0f;

        void prepare (double newFsOversampled) noexcept;
        void reset() noexcept;

        /** PHASE 2.10 — OS factor changed mid-note: recompute the decay coefficient for
            the new rate from the stored `decayMs`, and rescale the in-samples counters so
            the wall-clock timing is preserved. The running envelope `value` is KEPT (the
            kick does not restart). Coefficient-only. */
        void updateOversampledRate (double newFsOversampled) noexcept;

        /** Arm the envelope: raised-cosine attack then exponential decay of `newDecayMs`. */
        void noteOn (float newDecayMs) noexcept;

        float tick() noexcept;
        bool  isActive() const noexcept { return running; }

    private:
        double fsOversampled { 44100.0 };

        float  decayMs       { 400.0f };   // stored for Phase 2.10 coefficient refresh
        float  decayCoef     { 0.0f };
        float  value         { 0.0f };
        float  floorGain     { 0.00003162f };   // -90 dB

        int    attackSamples { 0 };
        int    attackPos     { 0 };
        int    samplesSinceTrigger { 0 };
        int    minLengthSamples    { 0 };
        int    maxLengthSamples    { 0 };

        bool   attacking { false };
        bool   running   { false };
    };
}
