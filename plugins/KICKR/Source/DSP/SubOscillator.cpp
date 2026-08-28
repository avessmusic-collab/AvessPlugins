#include "DSP/SubOscillator.h"
#include "Utilities/DSPUtils.h"

#include <cmath>
#include <limits>

namespace kickr
{
    void SubOscillator::prepare (double fsOversampled) noexcept
    {
        fs       = juce::jmax (1.0, fsOversampled);
        phaseInc = juce::MathConstants<double>::twoPi
                 * static_cast<double> (freqHz) / fs;
        decayCoef = dsputils::expDecayCoef (decayMs, fs);
        reset();
    }

    void SubOscillator::reset() noexcept
    {
        phase               = 0.0;
        env                 = 0.0f;
        attackPos           = 0;
        samplesSinceTrigger = 0;
        attacking           = false;
        active              = false;
    }

    void SubOscillator::setParams (float subLevel, float subFreqHz, float subDecayMs) noexcept
    {
        level   = juce::jlimit (0.0f, 1.0f, subLevel);
        decayMs = juce::jlimit (1.0f, 5000.0f, subDecayMs);

        // Fixed sub frequency — independent of the pitch envelope / fundamental / tune.
        // Clamp well outside the 25-80 Hz UI range, then refresh the increment vs
        // fsOversampled (per-block is fine).
        freqHz   = juce::jlimit (10.0f, 500.0f, subFreqHz);
        phaseInc = juce::MathConstants<double>::twoPi
                 * static_cast<double> (freqHz) / fs;

        decayCoef = dsputils::expDecayCoef (decayMs, fs);
    }

    void SubOscillator::noteOn() noexcept
    {
        phase = 0.0;   // research S5 — phase determinism (always, even when the layer is off)

        // Layer off -> do no work and don't extend the voice.
        if (level < 1.0e-6f)
        {
            reset();
            return;
        }

        decayCoef = dsputils::expDecayCoef (decayMs, fs);

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

    float SubOscillator::renderSample() noexcept
    {
        if (! active)
            return 0.0f;

        // sin() BEFORE advancing the phase -> first sample is exactly sin(0) = 0.
        const auto s = static_cast<float> (std::sin (phase));

        phase += phaseInc;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;
        else if (phase < 0.0)
            phase += juce::MathConstants<double>::twoPi;

        // Own AD envelope: raised-cosine attack -> one-pole exponential decay.
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

        return dsputils::sanitize (s * env * level);
    }
}
