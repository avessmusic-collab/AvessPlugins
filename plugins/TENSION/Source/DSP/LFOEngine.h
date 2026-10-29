#pragma once

#include <cstdint>

// LFOEngine - custom phase-accumulator LFO (Phase 3.2).
//
//  * Shapes: sine, triangle, saw (rising), reverse saw, sample & hold. Output is bipolar [-1, 1].
//  * S&H: deterministic xorshift32 (never juce::Random / std::rand), new value at every cycle
//    wrap, 2 ms one-pole slew so there are no clicks. setSeed() + reset() reproduce the sequence.
//  * Free mode : setTargetRateHz(hz).
//    Sync mode : setSyncTarget(bpm, cyclesPerBeat) -> rateHz = (bpm / 60) * snapped(cyclesPerBeat),
//                where the cycles-per-beat value is soft-snapped toward the power-of-two ladder
//                (1/8 ... 4 per beat) in the log2 domain with strength 0.6:
//                    x' = x - strength * sin(2*pi*x) / (2*pi),   x = log2(cyclesPerBeat)
//                (monotone for strength < 1, passes exactly through every integer = ladder rung,
//                 slope 1 - strength at the rungs = "dwell", 1 + strength between them).
//  * Phase is INTEGRATED (phase += rate / fs) so rate changes never cause a phase jump. The only
//    absolute alignment is alignToPpq(), meant to be called once on a transport start.
//  * Rate is slewed in the log2 domain (120 ms) by controlStep(), which the engine calls once per
//    32-sample control period; phaseInc is constant between control steps.
//  * Zero allocation anywhere.
class LFOEngine
{
public:
    enum class Shape : int { Sine = 0, Triangle, Saw, ReverseSaw, SampleHold };

    static constexpr double kMinRateHz     = 0.01;
    static constexpr double kMaxRateHz     = 40.0;
    static constexpr double kRateSlewSec   = 0.120;
    static constexpr double kSHSlewSec     = 0.002;
    static constexpr double kSnapStrength  = 0.6;

    void prepare (double sampleRate);

    // Phase <- phaseOffset, S&H sequence restarted from the seed, rate snapped to its target.
    void reset();

    void setSeed (std::uint32_t newSeed) noexcept;               // takes effect at the next reset()
    void setShape (Shape s) noexcept                { shape = s; }
    void setPhaseOffset (float cycles) noexcept     { phaseOffset = cycles - (float) (int) cycles; }
    void setRateScale (float s) noexcept            { rateScale = s; }   // call BEFORE the rate setters

    // Free mode
    void setTargetRateHz (float hz) noexcept;

    // Sync mode (tempo fallback is the caller's job: pass 120 when the host gives none)
    void setSyncTarget (double bpm, double cyclesPerBeat) noexcept;

    static double snapCyclesPerBeat (double cyclesPerBeat, double strength) noexcept;
    static double syncRateHz (double bpm, double cyclesPerBeat, double strength = kSnapStrength) noexcept;

    void snapRateToTarget() noexcept;
    void controlStep (int numSamples) noexcept;                  // advances the log-domain rate slew

    // One-shot re-alignment on transport start: phase = frac(ppq * cyclesPerBeat) + offset.
    void alignToPpq (double ppq) noexcept;

    float value() const noexcept;                                // output at the current phase (no advance)
    float next() noexcept;                                       // returns value(), then advances one sample
    void  advance (int numSamples) noexcept { for (int i = 0; i < numSamples; ++i) (void) next(); }

    float  getPhase() const noexcept                { return (float) phase; }
    double getCurrentRateHz() const noexcept;
    double getEffectiveCyclesPerBeat() const noexcept { return lastCyclesPerBeat; }

private:
    void  drawSampleHold() noexcept;
    std::uint32_t nextRandom() noexcept;

    double sampleRate = 44100.0;
    double phase = 0.0, phaseInc = 0.0;
    double log2Rate = 0.0, log2Target = 0.0;
    double lastCyclesPerBeat = 1.0;
    double shCoef = 0.1;

    Shape shape = Shape::Sine;
    float phaseOffset = 0.0f;
    float rateScale = 1.0f;

    std::uint32_t seed = 0x1234ABCDu, rng = 0x1234ABCDu;
    float shTarget = 0.0f, shValue = 0.0f;
};
