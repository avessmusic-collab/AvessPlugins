#include "DSP/NoiseGenerator.h"
#include "Utilities/DSPUtils.h"

#include <cmath>
#include <limits>

namespace kickr
{
    void NoiseGenerator::prepare (double fsOversampled) noexcept
    {
        fs           = juce::jmax (1.0, fsOversampled);
        nyquistLimit = static_cast<float> (fs * 0.45);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);   // per-sample processing
        spec.numChannels      = static_cast<juce::uint32> (1);

        bp.prepare (spec);
        bp.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        bp.setResonance (1.5f);

        updateDerived();
        bp.setCutoffFrequency (bpCentreHz);

        reset();
    }

    void NoiseGenerator::reset() noexcept
    {
        tiltState           = 0.0f;
        pk.fill (0.0f);
        env                 = 0.0f;
        attackPos           = 0;
        samplesSinceTrigger = 0;
        attacking           = false;
        active              = false;
        bp.reset();
    }

    void NoiseGenerator::updateDerived() noexcept
    {
        decayCoef = dsputils::expDecayCoef (decayMs, fs);

        // First-order tilt pivot one-pole coefficient (~1.5 kHz).
        const float w = juce::MathConstants<float>::twoPi
                      * kTiltPivotHz / static_cast<float> (fs);
        tiltCoef = juce::jlimit (0.0f, 1.0f, 1.0f - std::exp (-w));

        // Filtered: logarithmic 200 Hz -> 12 kHz band-pass centre, clamped to a safe range.
        const float mapped = 200.0f * std::pow (12000.0f / 200.0f, tone01);
        bpCentreHz = juce::jlimit (20.0f, nyquistLimit, mapped);
    }

    void NoiseGenerator::setParams (float noiseLevel, float noiseDecayMs,
                                    float noiseTone01, int noiseType) noexcept
    {
        level   = juce::jlimit (0.0f, 1.0f, noiseLevel);
        decayMs = juce::jlimit (5.0f, 1000.0f, noiseDecayMs);   // UI range 20-500
        tone01  = juce::jlimit (0.0f, 1.0f, noiseTone01);
        type    = juce::jlimit (0, 2, noiseType);

        updateDerived();
        bp.setCutoffFrequency (bpCentreHz);
    }

    void NoiseGenerator::noteOn() noexcept
    {
        // Deterministic noise — identical every trigger, reproducible offline renders.
        rng.setSeed (kSeed);

        // Layer off -> true bypass: do no work and don't extend the voice.
        // (The common case: noiseLevel defaults to 0.)
        if (level < 1.0e-6f)
        {
            reset();
            return;
        }

        updateDerived();
        bp.setCutoffFrequency (bpCentreHz);
        bp.reset();

        tiltState = 0.0f;
        pk.fill (0.0f);

        attackSamples = juce::jmax (1, static_cast<int> (std::lround (
                            static_cast<double> (kAttackMs) * 0.001 * fs)));
        attackPos     = 0;

        // Minimum length: attack + 5 ms guard so isActive() can't report "done" during
        // the ramp of a very short decay.
        minLengthSamples = attackSamples + static_cast<int> (std::lround (0.005 * fs));

        // Hard fallback: a one-pole -60 dB/decayMs tail is past -90 dB by ~1.5x decayMs,
        // so 2x decayMs + 50 ms always covers a fully-decayed layer.
        maxLengthSamples = static_cast<int> (std::lround (
                               (static_cast<double> (decayMs) * 2.0 + 50.0) * 0.001 * fs));

        samplesSinceTrigger = 0;
        env       = 0.0f;
        attacking = true;
        active    = true;
    }

    float NoiseGenerator::tilt (float x) noexcept
    {
        tiltState += tiltCoef * (x - tiltState);
        dsputils::flushDenormal (tiltState);

        const float lowBand  = tiltState;
        const float highBand = x - lowBand;

        // tone 0 -> 2*low (dark), 0.5 -> low + high = x (flat), 1 -> 2*high (bright).
        return 2.0f * (1.0f - tone01) * lowBand + 2.0f * tone01 * highBand;
    }

    float NoiseGenerator::pink (float white) noexcept
    {
        // Paul Kellet 7-pole approximation (standard coefficients).
        pk[0] = 0.99886f * pk[0] + white * 0.0555179f;
        pk[1] = 0.99332f * pk[1] + white * 0.0750759f;
        pk[2] = 0.96900f * pk[2] + white * 0.1538520f;
        pk[3] = 0.86650f * pk[3] + white * 0.3104856f;
        pk[4] = 0.55000f * pk[4] + white * 0.5329522f;
        pk[5] = -0.7616f * pk[5] - white * 0.0168980f;

        const float out = pk[0] + pk[1] + pk[2] + pk[3] + pk[4] + pk[5] + pk[6]
                        + white * 0.5362f;

        pk[6] = white * 0.115926f;

        for (auto& s : pk)
            dsputils::flushDenormal (s);

        return out * kPinkScale;
    }

    float NoiseGenerator::renderSample() noexcept
    {
        if (! active)
            return 0.0f;

        const float w = rng.nextFloat() * 2.0f - 1.0f;

        float n = 0.0f;
        switch (type)
        {
            case 1:  n = tilt (pink (w));         break;   // Pink
            case 2:  n = bp.processSample (0, w); break;   // Filtered
            default: n = tilt (w);                break;   // White
        }

        if (type == 2)
            bp.snapToZero();

        // AD envelope: raised-cosine attack -> one-pole exponential decay.
        if (attacking)
        {
            const auto x = static_cast<float> (attackPos)
                         / static_cast<float> (attackSamples);
            env = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * x);

            if (++attackPos >= attackSamples)
            {
                attacking = false;
                env       = 1.0f;
            }
        }
        else
        {
            env *= decayCoef;
        }

        dsputils::flushDenormal (env);

        if (samplesSinceTrigger < std::numeric_limits<int>::max())
            ++samplesSinceTrigger;

        if (! attacking
            && samplesSinceTrigger > minLengthSamples
            && (env < kFloorGain || samplesSinceTrigger > maxLengthSamples))
        {
            active = false;
            env    = 0.0f;
        }

        return dsputils::sanitize (n * env * level);
    }
}
