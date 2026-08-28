#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.1 / 2.3).

        Body-layer amplitude: fixed-fast 2.0 ms raised-cosine attack (kBodyAttackMs —
        `bodyAttack` is deliberately NOT a parameter, Open Q 9), one-pole exponential
        decay `coef = exp(-6.9078 / (bodyDecay_s * fs))`, floored/cut at -90 dB
        (feeds KickVoice::isActive()).
    */
    class AmplitudeEnvelope
    {
    public:
        static constexpr float kBodyAttackMs = 2.0f;

        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept { value = 0.0f; }

        bool isActive() const noexcept { return false; }

    private:
        double fsOversampled { 44100.0 };
        float  value { 0.0f };
    };
}
