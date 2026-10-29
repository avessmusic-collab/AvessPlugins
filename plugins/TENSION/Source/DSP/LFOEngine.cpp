#include "LFOEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double kTwoPi = 6.283185307179586476925286766559;

    inline double clampRate (double hz) noexcept
    {
        return std::clamp (hz, LFOEngine::kMinRateHz, LFOEngine::kMaxRateHz);
    }
}

void LFOEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    shCoef = 1.0 - std::exp (-1.0 / (kSHSlewSec * sampleRate));
    reset();
}

void LFOEngine::setSeed (std::uint32_t newSeed) noexcept
{
    seed = newSeed != 0u ? newSeed : 0x9E3779B9u;   // xorshift32 must never hold 0
}

std::uint32_t LFOEngine::nextRandom() noexcept
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

void LFOEngine::drawSampleHold() noexcept
{
    shTarget = (float) ((nextRandom() >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f;   // [-1, 1)
}

void LFOEngine::reset()
{
    rng   = seed;
    phase = (double) phaseOffset;

    drawSampleHold();
    shValue = shTarget;          // first plateau is reached without a slew

    snapRateToTarget();
}

void LFOEngine::setTargetRateHz (float hz) noexcept
{
    lastCyclesPerBeat = 0.0;
    log2Target = std::log2 (clampRate ((double) hz * (double) rateScale));
}

double LFOEngine::snapCyclesPerBeat (double cyclesPerBeat, double strength) noexcept
{
    if (cyclesPerBeat <= 0.0)
        return cyclesPerBeat;

    const double x = std::log2 (cyclesPerBeat);
    const double snapped = x - strength * std::sin (kTwoPi * x) / kTwoPi;
    return std::exp2 (snapped);
}

double LFOEngine::syncRateHz (double bpm, double cyclesPerBeat, double strength) noexcept
{
    return (bpm / 60.0) * snapCyclesPerBeat (cyclesPerBeat, strength);
}

void LFOEngine::setSyncTarget (double bpm, double cyclesPerBeat) noexcept
{
    const double effectiveCpb = snapCyclesPerBeat (cyclesPerBeat, kSnapStrength) * (double) rateScale;
    lastCyclesPerBeat = effectiveCpb;
    log2Target = std::log2 (clampRate ((bpm / 60.0) * effectiveCpb));
}

void LFOEngine::snapRateToTarget() noexcept
{
    log2Rate = log2Target;
    phaseInc = std::exp2 (log2Rate) / sampleRate;
}

void LFOEngine::controlStep (int numSamples) noexcept
{
    const double diff = log2Target - log2Rate;

    if (std::abs (diff) < 1.0e-7)
        log2Rate = log2Target;
    else
        log2Rate += diff * (1.0 - std::exp (-(double) numSamples / (kRateSlewSec * sampleRate)));

    phaseInc = std::exp2 (log2Rate) / sampleRate;
}

void LFOEngine::alignToPpq (double ppq) noexcept
{
    if (lastCyclesPerBeat <= 0.0)
        return;   // free mode: nothing to align to

    const double p = ppq * lastCyclesPerBeat + (double) phaseOffset;
    phase = p - std::floor (p);
}

double LFOEngine::getCurrentRateHz() const noexcept
{
    return std::exp2 (log2Rate);
}

float LFOEngine::value() const noexcept
{
    const float p = (float) phase;

    switch (shape)
    {
        case Shape::Sine:       return (float) std::sin (kTwoPi * phase);
        case Shape::Triangle:   return p < 0.25f ? 4.0f * p : (p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f);
        case Shape::Saw:        return 2.0f * p - 1.0f;
        case Shape::ReverseSaw: return 1.0f - 2.0f * p;
        case Shape::SampleHold: return shValue;
    }
    return 0.0f;
}

float LFOEngine::next() noexcept
{
    const float y = value();

    phase += phaseInc;

    if (phase >= 1.0)
    {
        phase -= std::floor (phase);
        drawSampleHold();
    }

    if (shape == Shape::SampleHold)
        shValue += (float) shCoef * (shTarget - shValue);

    return y;
}
