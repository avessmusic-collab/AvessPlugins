#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

// OutputSafety - custom (NO juce::dsp::Limiter: it hides a fixed 4:1 compressor).
//
//  1. Scrub     : non-finite sample -> 0, counted, raises a "reset feedback modules" flag.
//  2. Soft knee : exact identity for |x| <= 0.891 (-1 dBFS); above, a C1-continuous
//                 tanh knee with asymptote 1.0. Blended in by the safety engagement s
//                 (s == 0 => exact identity, even for > 0 dBFS input).
//  3. Guard     : slow wet-bus peak tracker (5 ms attack / 200 ms release). If the wet
//                 bus sits above +9 dBFS for > 100 ms the feedback clamp (1.0 -> 0.5)
//                 engages (consumed by delay/reverb in later phases; skeleton only here).
//
// Output trim is NOT part of this class; the engine applies it AFTER safety.
class OutputSafety
{
public:
    static constexpr float kneeStart       = 0.891f;      // -1 dBFS
    static constexpr float guardThreshold  = 2.8183829f;  // +9 dBFS
    static constexpr float guardRecoverRatio = 0.7f;      // hysteresis
    static constexpr float guardClampGain  = 0.5f;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    // Bit test, independent of fast-math settings.
    static bool isFinite (float x) noexcept
    {
        return (std::bit_cast<std::uint32_t> (x) & 0x7f800000u) != 0x7f800000u;
    }

    // Pure soft-knee curve (no blend). Identity for |x| <= kneeStart.
    static float softKnee (float x) noexcept;

    // Scrub + knee blended by s in [0,1]. s <= 0 returns x untouched (bit-exact).
    float processSample (float x, float s) noexcept
    {
        if (! isFinite (x))
        {
            nonFiniteCount.fetch_add (1, std::memory_order_relaxed);
            resetRequested.store (true, std::memory_order_relaxed);
            return 0.0f;
        }

        if (s <= 0.0f)
            return x;

        if (std::abs (x) <= kneeStart)
            return x;

        return x + s * (softKnee (x) - x);
    }

    // Runaway guard: call once per sample with max(|wetL|, |wetR|).
    void updateGuard (float wetPeakAbs) noexcept;

    bool  isGuardActive() const noexcept       { return guardActive; }
    float getFeedbackClamp() const noexcept    { return feedbackClamp; }   // 1.0 normal, 0.5 clamped
    float getGuardEnvelope() const noexcept    { return guardEnv; }

    // True once per scrub event batch; the engine resets feedback modules at the next block boundary.
    bool consumeResetRequest() noexcept        { return resetRequested.exchange (false, std::memory_order_relaxed); }

    int  getNonFiniteCount() const noexcept    { return nonFiniteCount.load (std::memory_order_relaxed); }

private:
    std::atomic<int>  nonFiniteCount { 0 };
    std::atomic<bool> resetRequested { false };

    // Audio-thread-only guard state
    float guardEnv     = 0.0f;
    int   overCount    = 0;
    bool  guardActive  = false;
    float feedbackClamp = 1.0f;

    float attackCoef  = 0.0f;
    float releaseCoef = 0.0f;
    float clampCoef   = 0.0f;
    int   holdSamples = 4800;
};
