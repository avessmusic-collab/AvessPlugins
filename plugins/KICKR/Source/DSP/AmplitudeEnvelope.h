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
          length — feeds KickVoice::isActive().

        Ticks at `fsOversampled` (AD-10). All state denormal-flushed.
    */
    class AmplitudeEnvelope
    {
    public:
        static constexpr float kBodyAttackMs = 2.0f;
        static constexpr float kFloorDb      = -90.0f;

        void prepare (double newFsOversampled) noexcept;
        void reset() noexcept;

        /** Arm the envelope: raised-cosine attack then exponential decay of `decayMs`. */
        void noteOn (float decayMs) noexcept;

        float tick() noexcept;
        bool  isActive() const noexcept { return running; }

    private:
        double fsOversampled { 44100.0 };

        float  decayCoef     { 0.0f };
        float  value         { 0.0f };
        float  floorGain     { 0.00003162f };   // -90 dB

        int    attackSamples { 0 };
        int    attackPos     { 0 };
        int    samplesSinceTrigger { 0 };
        int    minLengthSamples    { 0 };

        bool   attacking { false };
        bool   running   { false };
    };
}
