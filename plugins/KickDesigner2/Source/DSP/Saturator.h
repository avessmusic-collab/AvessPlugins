#pragma once

#include <cmath>

namespace kd2
{
    /**
        Stage 1 stub. Implemented in Stage 2 (plan.md Phase 2.6 / 2.8).

        Shared per-layer transfer functions (branch-free), used by BodyOscillator
        (`bodyHarmonics`) and TailGenerator (`tailDrive`). Always evaluated inside the
        OS region (AD-10). The master morphing distortion lives in Waveshaper.h.
    */
    namespace saturator
    {
        inline float tanhShape (float x) noexcept { return std::tanh (x); }

        // lerp(sine, tanh(kH*sine)/tanh(kH), amount), kH in [1, 4]
        inline float bodyHarmonics (float s, float amount01) noexcept
        {
            const float kH = 1.0f + 3.0f * amount01;
            const float shaped = std::tanh (kH * s) / std::tanh (kH);
            return s + amount01 * (shaped - s);
        }
    }
}
