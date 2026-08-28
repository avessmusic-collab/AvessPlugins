#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kickr
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.2).

        Normalised-exponential snap+settle contour (Open Q 5 RESOLVED):
            tau = clamp(tSinceTrigger / pitchTime, 0, 1)
            e(tau) = (exp(-k*tau) - exp(-k)) / (1 - exp(-k)),  k = 0.6 + pitchCurve*8.4
            f(t)   = fundamentalEff * pitchStartEff ^ e(tau)
        Ratio/log domain only — never lerp Hz. Phase integrated downstream in BodyOscillator.
    */
    class PitchEnvelope
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec) noexcept { fsOversampled = spec.sampleRate; }
        void reset() noexcept { samplesSinceTrigger = 0; }

    private:
        double fsOversampled { 44100.0 };
        int    samplesSinceTrigger { 0 };
    };
}
