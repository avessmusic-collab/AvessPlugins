#include "DSP/AmplitudeEnvelope.h"
#include "Utilities/DSPUtils.h"

#include <cmath>
#include <limits>

namespace kickr
{
    void AmplitudeEnvelope::prepare (double fsOS) noexcept
    {
        fsOversampled = juce::jmax (1.0, fsOS);
        floorGain     = juce::Decibels::decibelsToGain (kFloorDb);
        reset();
    }

    void AmplitudeEnvelope::reset() noexcept
    {
        value              = 0.0f;
        attackPos          = 0;
        samplesSinceTrigger = 0;
        attacking          = false;
        running            = false;
    }

    void AmplitudeEnvelope::noteOn (float decayMs) noexcept
    {
        decayCoef = dsputils::expDecayCoef (decayMs, fsOversampled);

        attackSamples = juce::jmax (1, static_cast<int> (std::lround (
                            kBodyAttackMs * 0.001 * fsOversampled)));
        attackPos     = 0;

        // Minimum voice length: the attack plus a short guard so isActive() can't
        // report "done" during the initial ramp of a very short decay.
        minLengthSamples = attackSamples + static_cast<int> (std::lround (0.005 * fsOversampled));

        // Hard fallback: a one-pole -60 dB/decayMs tail is past -90 dB by ~1.5x decayMs,
        // so 2x decayMs + 50 ms always covers a fully-decayed voice. Guarantees isActive()
        // flips false under machine-gun retriggering regardless of denormal flushing.
        maxLengthSamples = static_cast<int> (std::lround (
                               (static_cast<double> (decayMs) * 2.0 + 50.0) * 0.001 * fsOversampled));

        samplesSinceTrigger = 0;
        value     = 0.0f;
        attacking = true;
        running   = true;
    }

    float AmplitudeEnvelope::tick() noexcept
    {
        if (! running)
            return 0.0f;

        if (attacking)
        {
            const auto x = static_cast<float> (attackPos) / static_cast<float> (attackSamples);
            value = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * x);   // raised cosine 0 -> 1

            if (++attackPos >= attackSamples)
            {
                attacking = false;
                value     = 1.0f;
            }
        }
        else
        {
            value *= decayCoef;
        }

        dsputils::flushDenormal (value);
        if (samplesSinceTrigger < std::numeric_limits<int>::max())
            ++samplesSinceTrigger;

        if (! attacking
            && samplesSinceTrigger > minLengthSamples
            && (value < floorGain || samplesSinceTrigger > maxLengthSamples))
        {
            running = false;
            value   = 0.0f;
        }

        return value;
    }
}
