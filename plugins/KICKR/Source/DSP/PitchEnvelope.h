#pragma once

#include <cmath>
#include <juce_core/juce_core.h>

#include "Utilities/DSPUtils.h"

namespace kickr
{
    /**
        Pitch-drop contour (Open Q 5 RESOLVED — single `pitchCurve`-driven snap+settle).

            tau    = clamp(tSinceTrigger / pitchTime_seconds, 0, 1)
            k      = 0.6 + pitchCurve * 8.4                         (kMin 0.6 .. kMax 9.0)
            e(tau) = (exp(-k*tau) - exp(-k)) / (1 - exp(-k))        in [1, 0], exactly 0 at tau = 1
            f(t)   = fundamentalEff * pitchStart ^ e(tau)           (ratio/log domain — NEVER lerp Hz)
            tau >= 1  ->  f = fundamentalEff  (contour done; body just holds the fundamental)

        Frequency only — phase is integrated downstream in BodyOscillator and is never
        discontinuously reset mid-fall, so the drop is click-free by construction.

        Runs inside the OS region (AD-10): `tSinceTrigger` advances by `1 / fsOversampled`
        per sample. `k`, `exp(-k)`, `1/(1-exp(-k))` and `log2(pitchStart)` are computed once
        per block in `setParams()`; per sample costs one `std::exp` + one `std::exp2`.
    */
    class PitchEnvelope
    {
    public:
        PitchEnvelope() noexcept { setParams (4.0f, 50.0f, 0.7f); }   // spec defaults

        void prepare (double newFsOversampled) noexcept
        {
            fsOversampled    = juce::jmax (1.0, newFsOversampled);
            invFsOversampled = 1.0 / fsOversampled;
            reset();
        }

        void reset() noexcept { tSinceTrigger = 0.0; }

        /** Arm the contour — restart the elapsed-time accumulator (call on every trigger). */
        void noteOn() noexcept { tSinceTrigger = 0.0; }

        /**
            Per-block coefficient refresh (called once per block from KickEngine — the
            envelope never reads APVTS itself).

            @param startRatioEff  effective pitchStart (>= 1; velocity scaling already folded in)
            @param timeMs         pitchTime in milliseconds
            @param curve          pitchCurve in [0, 1]
        */
        void setParams (float startRatioEff, float timeMs, float curve) noexcept
        {
            const float c = juce::jlimit (0.0f, 1.0f, curve);
            k        = 0.6f + c * 8.4f;
            expNegK  = std::exp (-k);
            invDenom = 1.0f / juce::jmax (1.0e-9f, 1.0f - expNegK);

            const float startRatio = juce::jmax (1.0f, startRatioEff);
            log2StartRatio = std::log2 (startRatio);

            pitchTimeSeconds = juce::jmax (1.0e-4, static_cast<double> (timeMs) * 0.001);
        }

        /** Per sample: instantaneous body frequency (Hz) for `fundamentalEffHz`. */
        float nextFrequency (float fundamentalEffHz) noexcept
        {
            tSinceTrigger += invFsOversampled;

            const double tau = tSinceTrigger / pitchTimeSeconds;

            float f = fundamentalEffHz;
            if (tau < 1.0)
            {
                const float e = (std::exp (-k * static_cast<float> (tau)) - expNegK) * invDenom;
                f = fundamentalEffHz * std::exp2 (e * log2StartRatio);
            }

            return juce::jlimit (10.0f, 20000.0f, dsputils::sanitize (f));
        }

        /** Seconds since the last trigger. */
        double getElapsedSeconds() const noexcept { return tSinceTrigger; }

    private:
        double fsOversampled    { 44100.0 };
        double invFsOversampled { 1.0 / 44100.0 };
        double tSinceTrigger    { 0.0 };
        double pitchTimeSeconds { 0.05 };

        // Per-block contour coefficients (set in setParams()).
        float k              { 6.48f };
        float expNegK        { 0.0f };
        float invDenom       { 1.0f };
        float log2StartRatio { 2.0f };
    };
}
