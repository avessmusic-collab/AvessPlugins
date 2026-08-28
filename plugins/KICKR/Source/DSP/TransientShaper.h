#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.3).

        Kick-tuned attack/sustain enhancement on the summed signal. Dual one-pole
        envelope followers (fast ~1 ms atk / ~20 ms rel; slow ~15 ms / ~150 ms).
        Transient region when fast > slow * 1.05. attackGainDb = transientAttackEff * 6,
        sustainGainDb = transientSustain * 6. Position: after Tone(pre), before the
        master Waveshaper — inside the OS region (AD-10).
    */
    class TransientShaper
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept { fastEnv = 0.0f; slowEnv = 0.0f; }

    private:
        double fsOversampled { 44100.0 };
        float  fastEnv { 0.0f };
        float  slowEnv { 0.0f };
    };
}
