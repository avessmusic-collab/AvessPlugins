#pragma once

#include <cmath>

// AsymSmoother - one-pole smoother with separate attack (rising) and release (falling) time
// constants. Used for the reverb / delay RETURN gains (150/700 ms and 120/600 ms) so that dropping
// tension lets the tails ring out instead of being gated by the smoother.
//
// The value snaps EXACTLY onto the target once within 1e-7 of it, so a return that is told to go to
// 0 reaches an exact 0.0f (needed for the bit-exact t = 0 guarantee).
class AsymSmoother
{
public:
    void prepare (double sampleRate, double attackSec, double releaseSec) noexcept
    {
        attackCoef  = (float) (1.0 - std::exp (-1.0 / (attackSec  * sampleRate)));
        releaseCoef = (float) (1.0 - std::exp (-1.0 / (releaseSec * sampleRate)));
    }

    void setCurrentAndTarget (float v) noexcept { current = v; target = v; }
    void setTarget (float v) noexcept           { target = v; }

    float next() noexcept
    {
        if (current != target)
        {
            current += (target - current) * (target > current ? attackCoef : releaseCoef);

            if (std::abs (target - current) < 1.0e-7f)
                current = target;
        }

        return current;
    }

    float getCurrent() const noexcept { return current; }
    float getTarget() const noexcept  { return target; }
    bool  isSettled() const noexcept  { return current == target; }

private:
    float current = 0.0f, target = 0.0f;
    float attackCoef = 0.01f, releaseCoef = 0.001f;
};
