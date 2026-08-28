#pragma once

#include <juce_core/juce_core.h>

namespace kickr
{
    /**
        Pitch-drop contour (Open Q 5 RESOLVED).

        PHASE 2.1: PASSTHROUGH — `nextFrequency(base)` returns `base` unchanged so the
        body oscillator runs at the resolved fundamental. PHASE 2.2 fills in the
        normalised-exponential snap+settle:

            tau  = clamp(tSinceTrigger / pitchTime, 0, 1)
            e(tau) = (exp(-k*tau) - exp(-k)) / (1 - exp(-k)),   k = 0.6 + pitchCurve*8.4
            f(t) = fundamentalEff * pitchStartEff ^ e(tau)

        Ratio/log domain only — never lerp Hz. Phase integrated downstream in
        BodyOscillator. Runs inside the OS region (AD-10) — uses `fsOversampled`.
    */
    class PitchEnvelope
    {
    public:
        void prepare (double newFsOversampled) noexcept { fsOversampled = juce::jmax (1.0, newFsOversampled); reset(); }
        void reset() noexcept { samplesSinceTrigger = 0; }

        void noteOn() noexcept { samplesSinceTrigger = 0; }

        /** PHASE 2.1: identity. PHASE 2.2: apply the snap+settle contour to `baseHz`. */
        float nextFrequency (float baseHz) noexcept
        {
            ++samplesSinceTrigger;
            return baseHz;
        }

        /** Seconds since the last trigger (Phase 2.2 drives `tau = elapsed / pitchTime`). */
        double getElapsedSeconds() const noexcept
        {
            return static_cast<double> (samplesSinceTrigger) / fsOversampled;
        }

    private:
        double fsOversampled     { 44100.0 };
        int    samplesSinceTrigger { 0 };
    };
}
