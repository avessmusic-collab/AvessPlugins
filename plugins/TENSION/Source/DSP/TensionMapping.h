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

        // ---- PHASE 3.2 TEMPORARY LINEAR STUB destinations (Cinematic end values, linear in t) ----
        // Replaced by the real nonlinear per-destination curves + rise-mode tables in Phase 3.4.
        float inputTrim         = 1.0f;     // linear gain ahead of the pre-filter (identity until 3.4)
        float hpCutoffHz        = 20.0f;    // FilterEngine HP            20 -> 640 Hz
        float bpBlend           = 0.0f;     // FilterEngine BP blend       0 -> 0.30
        float bpCenterHz        = 300.0f;   // FilterEngine BP centre    300 -> 9000 Hz
        float bpQ               = 0.9f;     // FilterEngine BP resonance 0.9 -> 3.2 (engine caps at 4.0)
        float lfoRateHz         = 0.15f;    // LFO free-mode rate       0.15 -> 9 Hz
        float lfoCyclesPerBeat  = 0.125f;   // LFO sync-mode cycles/beat 1/8 -> 4
        float lfoAmpDepth       = 0.0f;     // tremolo depth              0 -> 0.35
        float lfoFilterDepthOct = 0.0f;     // lfoA -> cutoff, octaves    0 -> 0.6
        float lfoMoveDepth      = 0.0f;     // lfoB -> width sway/rotation 0 -> 0.5
        float satDrive          = 1.0f;     // SaturationEngine drive     1 -> 4.2
        float satAmount         = 0.0f;     // SaturationEngine parallel amount 0 -> 0.85
        float widthFactor       = 1.0f;     // StereoWidth highs width    1 -> 2.1
        float lowSideCollapse   = 0.0f;     // StereoWidth k (S_low *= 1 - 0.8k)  0 -> 1
    };

    namespace stub
    {
        inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
    }

    inline TensionTargets evaluateTensionMapping (float t, RiseMode /*mode*/) noexcept
    {
        const float tc = std::clamp (t, 0.0f, 1.0f);

        TensionTargets out;
        out.engagement        = curves::smooth (0.0f, 0.02f, tc);
        out.safety            = curves::smooth (0.0f, 0.03f, tc);
        out.placeholderLinear = tc;

        // PHASE 3.2 STUB: plain linear interpolation, identical for all three rise modes.
        out.inputTrim         = 1.0f;
        out.hpCutoffHz        = stub::lerp (20.0f,   640.0f,  tc);
        out.bpBlend           = stub::lerp (0.0f,    0.30f,   tc);
        out.bpCenterHz        = stub::lerp (300.0f,  9000.0f, tc);
        out.bpQ               = stub::lerp (0.9f,    3.2f,    tc);
        out.lfoRateHz         = stub::lerp (0.15f,   9.0f,    tc);
        out.lfoCyclesPerBeat  = stub::lerp (0.125f,  4.0f,    tc);
        out.lfoAmpDepth       = stub::lerp (0.0f,    0.35f,   tc);
        out.lfoFilterDepthOct = stub::lerp (0.0f,    0.6f,    tc);
        out.lfoMoveDepth      = stub::lerp (0.0f,    0.5f,    tc);
        out.satDrive          = stub::lerp (1.0f,    4.2f,    tc);
        out.satAmount         = stub::lerp (0.0f,    0.85f,   tc);
        out.widthFactor       = stub::lerp (1.0f,    2.1f,    tc);
        out.lowSideCollapse   = tc;
        return out;
    }
}
