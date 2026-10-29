#include "OutputSafety.h"

#include <algorithm>

void OutputSafety::prepare (double sampleRate) noexcept
{
    const double fs = sampleRate > 0.0 ? sampleRate : 44100.0;

    attackCoef  = (float) std::exp (-1.0 / (0.005 * fs));   //   5 ms
    releaseCoef = (float) std::exp (-1.0 / (0.200 * fs));   // 200 ms
    clampCoef   = (float) std::exp (-1.0 / (0.050 * fs));   //  50 ms clamp slew
    holdSamples = std::max (1, (int) (0.100 * fs));         // 100 ms

    reset();
}

void OutputSafety::reset() noexcept
{
    guardEnv      = 0.0f;
    overCount     = 0;
    guardActive   = false;
    feedbackClamp = 1.0f;
    resetRequested.store (false, std::memory_order_relaxed);
}

float OutputSafety::softKnee (float x) noexcept
{
    const float ax = std::abs (x);

    if (ax <= kneeStart)
        return x;

    const float range = 1.0f - kneeStart;
    const float y = kneeStart + range * std::tanh ((ax - kneeStart) / range);
    return std::copysign (y, x);
}

void OutputSafety::updateGuard (float wetPeakAbs) noexcept
{
    const float a = isFinite (wetPeakAbs) ? wetPeakAbs : 0.0f;

    const float coef = a > guardEnv ? attackCoef : releaseCoef;
    guardEnv = coef * guardEnv + (1.0f - coef) * a;

    if (guardEnv < 1.0e-20f)
        guardEnv = 0.0f;

    if (guardEnv > guardThreshold)
    {
        if (++overCount >= holdSamples)
            guardActive = true;
    }
    else
    {
        overCount = 0;

        if (guardEnv < guardThreshold * guardRecoverRatio)
            guardActive = false;
    }

    const float target = guardActive ? guardClampGain : 1.0f;
    feedbackClamp = target + clampCoef * (feedbackClamp - target);
}
