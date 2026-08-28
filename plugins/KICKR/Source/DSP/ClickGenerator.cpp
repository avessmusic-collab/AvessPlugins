#include "DSP/ClickGenerator.h"

#include <cmath>

namespace kickr
{
    void ClickGenerator::prepare (double fsOversampled) noexcept
    {
        fs           = juce::jmax (1.0, fsOversampled);
        nyquistLimit = static_cast<float> (fs * 0.45);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);   // per-sample processing
        spec.numChannels      = static_cast<juce::uint32> (1);

        const float toneInit = juce::jlimit (20.0f, nyquistLimit, 4000.0f);

        noiseFilter.prepare (spec);
        noiseFilter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        noiseFilter.setResonance (0.7f);
        noiseFilter.setCutoffFrequency (toneInit);

        impulseFilter.prepare (spec);
        impulseFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        impulseFilter.setResonance (2.0f);
        impulseFilter.setCutoffFrequency (toneInit);

        // Fixed normalised-exp pitch-drop coefficients for component B (curve fixed).
        expNegKDrop  = std::exp (-kPitchDropK);
        invDenomDrop = 1.0f / juce::jmax (1.0e-9f, 1.0f - expNegKDrop);

        // Raised-cosine impulse window: kClickImpulseWindowMs at fs, clamped to
        // [8, kWindowLutSize] samples — never a bare 1-sample spike (research S2).
        const int lenFromMs = static_cast<int> (std::lround (
                                  static_cast<double> (kClickImpulseWindowMs) * 0.001 * fs));
        impulseLen = juce::jlimit (8, static_cast<int> (kWindowLutSize),
                                   juce::jmax (8, lenFromMs));

        windowLut.fill (0.0f);
        for (int n = 0; n < impulseLen; ++n)
            windowLut[static_cast<size_t> (n)] =
                0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                        * static_cast<float> (n)
                                        / static_cast<float> (impulseLen - 1));

        cRingSamples = static_cast<int> (std::lround (0.003 * fs));   // ~3 ms LP ring-out

        reset();
    }

    void ClickGenerator::reset() noexcept
    {
        noiseFilter.reset();
        impulseFilter.reset();
        envA.reset();
        envB.reset();

        bPhase     = 0.0;
        bPos       = 0;
        impulsePos = 0;
        cActive    = false;
        active     = false;
    }

    void ClickGenerator::setParams (float level, float toneHz, float timeMs, float pitchHz) noexcept
    {
        clickLevel   = juce::jlimit (0.0f, 1.0f, level);
        clickTimeMs  = juce::jlimit (0.1f, 50.0f, timeMs);
        clickPitchHz = juce::jlimit (20.0f, nyquistLimit, pitchHz);

        const float toneClamped = juce::jlimit (20.0f, nyquistLimit, toneHz);
        noiseFilter.setCutoffFrequency (toneClamped);
        impulseFilter.setCutoffFrequency (toneClamped);

        pitchDropSamples = juce::jmax (1, static_cast<int> (std::lround (
                               static_cast<double> (juce::jmin (clickTimeMs, 8.0f)) * 0.001 * fs)));
    }

    void ClickGenerator::noteOn (float velClickGain) noexcept
    {
        velClick = juce::jmax (0.0f, velClickGain);

        // Layer off -> do no work and don't extend the voice.
        if (clickLevel < 1.0e-6f)
        {
            reset();
            return;
        }

        envA.arm (0.05f, clickTimeMs, fs);
        envB.arm (0.02f, 0.8f * clickTimeMs, fs);

        bPhase = 0.0;
        bPos   = 0;

        impulsePos = 0;
        cActive    = true;

        noiseFilter.reset();
        impulseFilter.reset();

        active = true;
    }

    float ClickGenerator::renderSample() noexcept
    {
        if (! active)
            return 0.0f;

        // A — filtered white-noise burst (weight 0.50).
        float a = 0.0f;
        if (envA.isActive())
        {
            const float w = rng.nextFloat() * 2.0f - 1.0f;
            a = noiseFilter.processSample (0, w) * envA.tick();
        }

        // B — transient sine with its own fast pitch drop (weight 0.35).
        // sin() BEFORE the phase increment so the first sample is exactly sin(0) = 0
        // (clean phase-0 start — no step discontinuity at the trigger).
        float b = 0.0f;
        if (envB.isActive())
        {
            const double tau = static_cast<double> (bPos)
                             / static_cast<double> (juce::jmax (1, pitchDropSamples));

            float e = 0.0f;
            if (tau < 1.0)
                e = (std::exp (-kPitchDropK * static_cast<float> (tau)) - expNegKDrop)
                        * invDenomDrop;

            // clickPitch -> 0.5*clickPitch  (2^e maps e in [0,1] to ratio [1,2]).
            const float freq = 0.5f * clickPitchHz * std::exp2 (e);

            b = std::sin (static_cast<float> (bPhase)) * envB.tick();

            bPhase += juce::MathConstants<double>::twoPi
                        * static_cast<double> (freq) / fs;
            if (bPhase >= juce::MathConstants<double>::twoPi)
                bPhase -= juce::MathConstants<double>::twoPi;
            ++bPos;
        }

        // C — windowed raised-cosine impulse -> resonant low-pass (weight 0.15).
        float c = 0.0f;
        if (cActive)
        {
            const float imp = (impulsePos < impulseLen)
                                ? windowLut[static_cast<size_t> (impulsePos)]
                                : 0.0f;
            c = impulseFilter.processSample (0, imp);

            if (++impulsePos >= impulseLen + cRingSamples)
                cActive = false;
        }

        noiseFilter.snapToZero();
        impulseFilter.snapToZero();

        if (! envA.isActive() && ! envB.isActive() && ! cActive)
            active = false;

        // Phase 2.9: clickWidth L/R decorrelation (2nd noise stream + <1 ms osc/impulse offset).
        const float mono = (0.50f * a + 0.35f * b + 0.15f * c) * clickLevel * velClick;
        return dsputils::sanitize (mono);
    }
}
