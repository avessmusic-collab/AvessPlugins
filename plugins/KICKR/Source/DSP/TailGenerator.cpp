#include "DSP/TailGenerator.h"
#include "Utilities/DSPUtils.h"

#include <cmath>
#include <limits>

namespace kickr
{
    void TailGenerator::prepare (double fsOversampled) noexcept
    {
        fs           = juce::jmax (1.0, fsOversampled);
        nyquistLimit = static_cast<float> (fs * 0.45);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);   // per-sample processing
        spec.numChannels      = static_cast<juce::uint32> (1);

        lp.prepare (spec);
        lp.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        lp.setResonance (0.7f);

        updateDerived();
        lp.setCutoffFrequency (lpCutoffHz);

        phaseInc = juce::MathConstants<double>::twoPi
                 * static_cast<double> (fundamentalHz) / fs;

        reset();
    }

    void TailGenerator::reset() noexcept
    {
        phase               = 0.0;
        env                 = 0.0f;
        attackPos           = 0;
        samplesSinceTrigger = 0;
        attacking           = false;
        active              = false;
        lp.reset();
    }

    void TailGenerator::updateOversampledRate (double newFsOversampled) noexcept
    {
        const double oldFs = fs;
        fs           = juce::jmax (1.0, newFsOversampled);
        nyquistLimit = static_cast<float> (fs * 0.45);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = fs;
        spec.maximumBlockSize = static_cast<juce::uint32> (32);
        spec.numChannels      = static_cast<juce::uint32> (1);
        lp.prepare (spec);   // same numChannels -> no allocation
        lp.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        lp.setResonance (0.7f);

        updateDerived();
        lp.setCutoffFrequency (lpCutoffHz);

        phaseInc = juce::MathConstants<double>::twoPi
                 * static_cast<double> (fundamentalHz) / fs;   // keep phase

        if (active)
        {
            const double r = fs / juce::jmax (1.0, oldFs);
            auto scale = [r] (int n) noexcept
            {
                return juce::jmax (0, static_cast<int> (std::llround (static_cast<double> (n) * r)));
            };
            attackSamples       = juce::jmax (1, scale (attackSamples));
            attackPos           = scale (attackPos);
            minLengthSamples    = scale (minLengthSamples);
            maxLengthSamples    = scale (maxLengthSamples);
            samplesSinceTrigger = scale (samplesSinceTrigger);
        }
    }

    void TailGenerator::updateDerived() noexcept
    {
        decayCoef = dsputils::expDecayCoef (lengthMs, fs);

        // tailTone: logarithmic 120 Hz -> 4 kHz, clamped to a safe SVF range.
        const float mapped = 120.0f * std::pow (4000.0f / 120.0f, tone01);
        lpCutoffHz = juce::jlimit (20.0f, nyquistLimit, mapped);

        // tailDrive: tanh pre-gain 1 -> 8. Parallel blend by drive01 keeps
        // drive01 = 0 bit-transparent; /tanh(g) normalisation is peak-preserving
        // so low levels are not just made quieter.
        driveGain        = 1.0f + drive01 * 7.0f;
        invTanhDriveGain = 1.0f / juce::jmax (1.0e-6f, std::tanh (driveGain));
        driveActive      = drive01 > 1.0e-6f;
    }

    void TailGenerator::setParams (float tailLevel, float tailLengthMs,
                                   float tailTone01, float tailDrive01) noexcept
    {
        level    = juce::jlimit (0.0f, 1.0f, tailLevel);
        lengthMs = juce::jlimit (5.0f, 5000.0f, tailLengthMs);   // UI range 20-2000
        tone01   = juce::jlimit (0.0f, 1.0f, tailTone01);
        drive01  = juce::jlimit (0.0f, 1.0f, tailDrive01);

        updateDerived();
        lp.setCutoffFrequency (lpCutoffHz);
    }

    void TailGenerator::noteOn (float fundamentalEffHz) noexcept
    {
        phase = 0.0;   // phase determinism (always, even when the layer is off)

        // Layer off -> true bypass: do no work and don't extend the voice.
        if (level < 1.0e-6f)
        {
            reset();
            return;
        }

        // Locked to the resolved fundamental the body got, BEFORE the pitch-envelope
        // multiplier (steady frequency for the whole note).
        fundamentalHz = juce::jlimit (10.0f, 5000.0f, fundamentalEffHz);
        phaseInc      = juce::MathConstants<double>::twoPi
                      * static_cast<double> (fundamentalHz) / fs;

        updateDerived();
        lp.setCutoffFrequency (lpCutoffHz);
        lp.reset();

        // ~10 ms raised-cosine attack so the tail sits behind the click / transient.
        attackSamples = juce::jmax (1, static_cast<int> (std::lround (
                            static_cast<double> (kAttackMs) * 0.001 * fs)));
        attackPos     = 0;

        // Minimum length: attack + 5 ms guard so isActive() can't report "done"
        // during the ramp of a very short decay.
        minLengthSamples = attackSamples + static_cast<int> (std::lround (0.005 * fs));

        // Hard fallback: a one-pole -60 dB/lengthMs tail is past -90 dB by ~1.5x
        // lengthMs, so 2x lengthMs + 50 ms always covers a fully-decayed layer.
        maxLengthSamples = static_cast<int> (std::lround (
                               (static_cast<double> (lengthMs) * 2.0 + 50.0) * 0.001 * fs));

        samplesSinceTrigger = 0;
        env       = 0.0f;
        attacking = true;
        active    = true;
    }

    float TailGenerator::renderSample() noexcept
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

        // osc -> env -> LP(tailTone) -> tanh(tailDrive) -> x tailLevel.
        float y = lp.processSample (0, s * env);

        if (driveActive)
        {
            const float shaped = std::tanh (driveGain * y) * invTanhDriveGain;
            y += drive01 * (shaped - y);       // parallel blend; drive01 = 0 -> y unchanged
        }

        lp.snapToZero();

        if (samplesSinceTrigger < std::numeric_limits<int>::max())
            ++samplesSinceTrigger;

        if (! attacking
            && samplesSinceTrigger > minLengthSamples
            && (env < kFloorGain || samplesSinceTrigger > maxLengthSamples))
        {
            active = false;
            env    = 0.0f;
        }

        return dsputils::sanitize (y * level);
    }
}
