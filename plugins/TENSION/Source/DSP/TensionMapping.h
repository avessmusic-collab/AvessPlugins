#pragma once

#include <algorithm>

// TensionMapping - converts (tension t in [0,1], RiseMode) into a flat struct of
// destination targets. Pure functions, no JUCE dependency, unit-testable without audio.
//
// PHASE 3.1 STUB: only the two engagement destinations are real (engagement `e`
// and safety blend `s`, both smoothstep per architecture.md). Everything else is a
// linear placeholder. The real per-destination nonlinear curves, the three rise-mode
// profile tables and the 600 ms mode crossfade arrive in Phase 3.4.
namespace TensionDSP
{
    enum class RiseMode : int
    {
        SlowBurn   = 0,
        Cinematic  = 1,
        Aggressive = 2
    };

    inline RiseMode riseModeFromIndex (int index) noexcept
    {
        switch (index)
        {
            case 0:  return RiseMode::SlowBurn;
            case 2:  return RiseMode::Aggressive;
            default: return RiseMode::Cinematic;
        }
    }

    namespace curves
    {
        // Smoothstep S(a,b,t): exactly 0 for t <= a, exactly 1 for t >= b.
        inline float smooth (float a, float b, float t) noexcept
        {
            const float x = std::clamp ((t - a) / (b - a), 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        }
    }

    struct TensionTargets
    {
        float engagement = 0.0f;         // e : wet-bus dry/proc crossfade   S(0, 0.02, t)
        float safety     = 0.0f;         // s : OutputSafety knee blend      S(0, 0.03, t)
        float placeholderLinear = 0.0f;  // Phase 3.1 linear placeholder for not-yet-built destinations
    };

    inline TensionTargets evaluateTensionMapping (float t, RiseMode /*mode*/) noexcept
    {
        const float tc = std::clamp (t, 0.0f, 1.0f);

        TensionTargets out;
        out.engagement        = curves::smooth (0.0f, 0.02f, tc);
        out.safety            = curves::smooth (0.0f, 0.03f, tc);
        out.placeholderLinear = tc;
        return out;
    }
}
