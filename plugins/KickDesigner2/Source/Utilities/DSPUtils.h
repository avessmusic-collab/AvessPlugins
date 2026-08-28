#pragma once

#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

/**
    Shared DSP primitives for KickDesigner2 (architecture.md -> DSPUtils).
    Header-only helpers + one out-of-line buffer utility (DSPUtils.cpp).
    All real-time-safe: no allocation, no locks.
*/
namespace kd2::dsputils
{
    /** Cheap denormal flush. */
    inline void flushDenormal (float& x) noexcept
    {
        x += 1.0e-20f;
        x -= 1.0e-20f;
    }

    /** NaN / Inf guard -> 0. */
    inline float sanitize (float x) noexcept
    {
        return std::isfinite (x) ? x : 0.0f;
    }

    inline float dbToGain (float dB) noexcept   { return juce::Decibels::decibelsToGain (dB); }
    inline float gainToDb (float gain) noexcept { return juce::Decibels::gainToDecibels (gain); }

    /** One-pole exponential decay coefficient for ~-60 dB over `timeMs`. */
    inline float expDecayCoef (float timeMs, double sampleRate) noexcept
    {
        const float t = juce::jmax (0.01f, timeMs) * 0.001f;
        return std::exp (-6.9077553f / (t * static_cast<float> (sampleRate)));
    }

    /** Frequency ratio for a semitone offset. */
    inline float pitchRatio (float semitones) noexcept
    {
        return std::exp2 (semitones / 12.0f);
    }

    /** Equal-power crossfade gains for x in [0, 1] (a: 1->0, b: 0->1). */
    inline void equalPowerGains (float x, float& a, float& b) noexcept
    {
        const float t = juce::jlimit (0.0f, 1.0f, x) * juce::MathConstants<float>::halfPi;
        a = std::cos (t);
        b = std::sin (t);
    }

    /** One-pole DC blocker (configurable pole radius R). */
    struct DCBlocker
    {
        float R  { 0.9995f };  // ~5 Hz at 48 kHz
        float x1 { 0.0f };
        float y1 { 0.0f };

        void reset() noexcept { x1 = 0.0f; y1 = 0.0f; }

        float process (float x) noexcept
        {
            const float y = x - x1 + R * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    /** Replace any non-finite sample in `buffer` with 0. (Out-of-line — DSPUtils.cpp) */
    void sanitizeBuffer (juce::AudioBuffer<float>& buffer) noexcept;
}
