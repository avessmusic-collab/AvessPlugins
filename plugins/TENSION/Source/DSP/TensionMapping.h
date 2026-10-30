#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// TensionMapping - converts (tension t in [0,1], RiseMode) into a flat struct of destination
// targets. Pure functions, header-only, no JUCE dependency: unit-testable without audio.
//
// PHASE 3.4: the real nonlinear mapping.
//   * Curve primitives (namespace curves): smoothstep S(a,b,t), expo E(t,k) = (e^{kt}-1)/(e^k-1),
//     power P(t,p), octave oct(f0, octaves, c) = f0 * 2^(octaves*c), dB-linear dB(lo,hi,c) -> dbToGain.
//     Nothing is linear in t; every destination is monotone in t (tested).
//   * Three rise-mode profiles are constexpr DATA TABLES (struct Profile), not code branches:
//     every destination carries its own (start, end, curve parameters) per mode. The Cinematic
//     table is the architecture.md "Master Tension Map" verbatim; Slow Burn / Aggressive apply the
//     "Rise mode deltas" table (onset shifts, exponent scaling x1.4 / x0.7, LFO max rates 4 / 9 / 16 Hz,
//     depths x0.7 / 1.0 / 1.5, noise ceilings -20 / -16 / -12 dBFS, sat drive 3.0 / 4.2 / 7.0, width
//     1.8 / 2.1 / 2.4, reverb and delay rows, feedback 0.55 / 0.62 / 0.72).
//   * ModeBlender: the 600 ms mode crossfade. It keeps a weight per profile (current mode ramps to 1,
//     the previous mode(s) ramp to 0, linear, 600 ms), evaluates every profile with a non-zero weight
//     and lerps the WHOLE TensionTargets struct. With a single previous mode this is exactly the
//     architecture's "old + new profile, modeBlend 0..1"; switching again mid-blend stays continuous
//     (a third weight) instead of jumping.
//   * Per-destination LFO shapes live in the profile too (struct LfoShapes). ModeBlender::shapeMix()
//     hands the engine the shapes to evaluate and their weights, so an LFO shape change is a
//     crossfade driven by the SAME blend weights (click-free; no zero-crossing switching needed).
//
// Exactness at t = 0: every mapped gain (noise gate, reverb / delay returns, sends, feedback, LFO
// depths, saturation amount, BP blend, side collapse) evaluates to EXACTLY 0.0f, every start value is
// identical across the three modes, and lerping equal values is exact -> the t = 0 bit-exact
// passthrough is preserved for any blend state.
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

    // Same numeric order as LFOEngine::Shape (the engine static_asserts this).
    enum class LfoShape : int { Sine = 0, Triangle = 1, Saw = 2, ReverseSaw = 3, SampleHold = 4 };

    struct LfoShapes
    {
        LfoShape amp;       // tremolo (series chain + noise)
        LfoShape filter;    // pre-filter cutoff
        LfoShape stereo;    // width sway + rotation
        LfoShape delay;     // delay-time wobble
    };

    // =======================================================================================
    // Curve primitives
    // =======================================================================================
    namespace curves
    {
        // Smoothstep S(a,b,t): exactly 0 for t <= a, exactly 1 for t >= b.
        inline float smooth (float a, float b, float t) noexcept
        {
            const float span = b - a;
            if (span <= 0.0f)
                return t >= b ? 1.0f : 0.0f;

            const float x = std::clamp ((t - a) / span, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        }

        // Exponential E(t,k) = (e^{kt} - 1) / (e^k - 1): exactly 0 at t = 0 and exactly 1 at t = 1.
        inline float expo (float t, float k) noexcept
        {
            const float x = std::clamp (t, 0.0f, 1.0f);
            if (x <= 0.0f) return 0.0f;
            if (x >= 1.0f) return 1.0f;
            if (std::abs (k) < 1.0e-6f) return x;
            return (std::exp (k * x) - 1.0f) / (std::exp (k) - 1.0f);
        }

        // Power P(t,p) on [0,1]: exactly 0 at 0 and exactly 1 at 1.
        inline float power (float t, float p) noexcept
        {
            const float x = std::clamp (t, 0.0f, 1.0f);
            if (x <= 0.0f) return 0.0f;
            if (x >= 1.0f) return 1.0f;
            return std::pow (x, p);
        }

        // Musical frequency mapping: f0 * 2^(octaves * c). c == 0 returns f0 exactly.
        inline float oct (float f0, float octaves, float c) noexcept
        {
            return f0 * std::exp2 (octaves * c);
        }

        inline float dbToGain (float db) noexcept { return std::pow (10.0f, db / 20.0f); }

        // dB-linear interpolation, then dbToGain.
        inline float dbLinear (float loDb, float hiDb, float c) noexcept
        {
            return dbToGain (loDb + (hiDb - loDb) * c);
        }
    }

    // =======================================================================================
    // Per-destination curve descriptions (the data the profile tables are made of)
    // =======================================================================================
    struct SCurve   { float start, end, lo, hi, p; };            // start + (end - start) * P(S(lo,hi,t), p)
    struct SOct     { float start, octaves, lo, hi, p; };        // start * 2^(octaves * P(S(lo,hi,t), p))
    struct EOct     { float start, octaves, k; };                // start * 2^(octaves * E(t, k))
    struct POct     { float start, octaves, p; };                // start * 2^(octaves * P(t, p))
    struct RetCurve { float ceilDb, lo, hi, p; };                // dbToGain(ceilDb) * P(S(lo,hi,t), p)
    struct NoiseLevel { float floorDb, ceilDb, k, gateLo, gateHi; };   // S(gate) * dB(floor, ceil, E(t,k))

    struct Profile
    {
        LfoShapes  shapes;

        // ---- series chain ----
        SCurve     satDrive;
        SCurve     satAmount;
        EOct       hp;
        SCurve     bpBlend;
        POct       bpCenter;
        SCurve     bpQ;
        EOct       lfoRate;              // free mode, Hz
        EOct       lfoCyclesPerBeat;     // sync mode
        SCurve     lfoAmpDepth;
        SCurve     lfoFilterDepthOct;
        SCurve     lfoMoveDepth;
        SCurve     lfoDelayDepthMs;
        SCurve     width;
        SCurve     lowSideCollapse;      // k (StereoWidth multiplies it by 0.8)

        // ---- noise ----
        NoiseLevel noiseLevel;
        SCurve     noiseBrightness;
        EOct       noiseHpf;
        POct       noiseTop;
        SCurve     noiseFiltered;

        // ---- reverb / delay ----
        SCurve     reverbSend;
        RetCurve   reverbReturn;
        SCurve     reverbRoom;
        SCurve     reverbDamping;
        SCurve     reverbWidth;
        SCurve     reverbPreDelayMs;
        SOct       reverbReturnLpf;
        SOct       reverbSendHpf;
        SCurve     delaySend;
        RetCurve   delayReturn;
        SCurve     delayFeedback;
        SOct       delayFbLpf;
    };

    // =======================================================================================
    // PROFILE TABLES (constexpr data). Initial design values; Phase 3.5 retunes them.
    //
    // Cinematic = architecture.md "Master Tension Map" verbatim. Every START value is identical in
    // all three tables (so a mode switch at t = 0 is a no-op); modes differ in curve onsets,
    // exponents and END values. Delta interpretation (architecture.md "Rise mode deltas"):
    //   * "onset +0.1 / -0.05": the smoothstep lower edge moves (clamped at 0, upper edge kept at the
    //     Cinematic value so the end value is still reached at t = 1; noise gate is a pure window so
    //     both edges move).
    //   * "exponent x1.4 / x0.7": expo k and power p of the modulation / noise / saturation curves.
    //   * LFO depths x0.7 / 1.0 / 1.5 apply to all four LFO depth destinations.
    //   Values the table does not state (filter ends, BP blend / Q ends, noise colour ends, sync
    //   cycles-per-beat ends, saturation amount end) are marked [design] and are tuning candidates.
    // =======================================================================================
    inline constexpr Profile kSlowBurn
    {
        .shapes = { LfoShape::Sine, LfoShape::Sine, LfoShape::Triangle, LfoShape::Sine },

        .satDrive          = { 1.0f, 3.0f, 0.25f, 0.95f, 1.96f },          // drive max 3.0, P(S,1.4*1.4)
        .satAmount         = { 0.0f, 0.70f, 0.25f, 0.95f, 1.82f },         // [design] end 0.70
        .hp                = { 20.0f, 5.0f, 2.4f },
        .bpBlend           = { 0.0f, 0.20f, 0.2f, 0.9f, 1.0f },            // [design] restrained
        .bpCenter          = { 300.0f, 4.9f, 1.6f },
        .bpQ               = { 0.9f, 2.6f, 0.2f, 0.9f, 1.0f },             // [design]
        .lfoRate           = { 0.15f, 4.737f, 2.8f },                      // max ~4 Hz, k 2.0*1.4
        .lfoCyclesPerBeat  = { 0.125f, 4.0f, 2.8f },                       // [design] 1/8 -> 2 cycles/beat
        .lfoAmpDepth       = { 0.0f, 0.245f, 0.18f, 0.85f, 1.4f },         // 0.35 x 0.7
        .lfoFilterDepthOct = { 0.0f, 0.42f, 0.20f, 0.90f, 1.4f },          // 0.6 x 0.7
        .lfoMoveDepth      = { 0.0f, 0.35f, 0.30f, 0.90f, 1.4f },          // 0.5 x 0.7
        .lfoDelayDepthMs   = { 0.0f, 0.63f, 0.50f, 1.00f, 1.4f },          // 0.9 x 0.7
        .width             = { 1.0f, 1.8f, 0.25f, 0.95f, 1.2f },
        .lowSideCollapse   = { 0.0f, 1.0f, 0.3f, 1.0f, 1.0f },

        .noiseLevel        = { -66.0f, -20.0f, 3.64f, 0.14f, 0.38f },      // ceiling -20, k 2.6*1.4, gate +0.1
        .noiseBrightness   = { 0.0f, 0.60f, 0.20f, 0.95f, 1.0f },          // [design] pink, filtered
        .noiseHpf          = { 80.0f, 5.0f, 2.8f },
        .noiseTop          = { 2500.0f, 2.2f, 1.96f },                     // [design] darker top (pink)
        .noiseFiltered     = { 0.0f, 0.35f, 0.20f, 0.95f, 1.0f },          // [design]

        .reverbSend        = { 0.0f, 1.0f, 0.0f, 0.1f, 1.0f },
        .reverbReturn      = { -7.0f, 0.05f, 0.55f, 1.0f },                // earlier onset, -7 dB
        .reverbRoom        = { 0.35f, 0.97f, 0.1f, 0.9f, 1.0f },           // size 0.97
        .reverbDamping     = { 0.7f, 0.3f, 0.1f, 0.9f, 1.0f },
        .reverbWidth       = { 0.6f, 1.0f, 0.2f, 0.9f, 1.0f },
        .reverbPreDelayMs  = { 0.0f, 60.0f, 0.3f, 1.0f, 1.0f },            // up to 60 ms
        .reverbReturnLpf   = { 14000.0f, -1.0f, 0.1f, 0.9f, 1.0f },
        .reverbSendHpf     = { 120.0f, 0.8745f, 0.1f, 0.9f, 1.0f },
        .delaySend         = { 0.0f, 1.0f, 0.0f, 0.1f, 1.0f },
        .delayReturn       = { -8.0f, 0.2f, 0.7f, 1.0f },
        .delayFeedback     = { 0.0f, 0.55f, 0.45f, 1.0f, 1.0f },
        .delayFbLpf        = { 9000.0f, -1.5f, 0.45f, 1.0f, 1.0f }
    };

    inline constexpr Profile kCinematic
    {
        .shapes = { LfoShape::Triangle, LfoShape::Sine, LfoShape::Sine, LfoShape::Sine },

        .satDrive          = { 1.0f, 4.2f, 0.15f, 0.95f, 1.4f },          // P(S(0.15,0.95,t),1.4)
        .satAmount         = { 0.0f, 0.85f, 0.15f, 0.95f, 1.3f },         // P(S(0.15,0.95,t),1.3)
        .hp                = { 20.0f, 5.0f, 2.4f },                       // oct(20, 5.0, E(t,2.4)) -> 640 Hz
        .bpBlend           = { 0.0f, 0.30f, 0.2f, 0.9f, 1.0f },           // S(0.2,0.9,t)*0.30
        .bpCenter          = { 300.0f, 4.9f, 1.6f },                      // oct(300, 4.9, P(t,1.6)) -> ~8.9 kHz
        .bpQ               = { 0.9f, 3.2f, 0.2f, 0.9f, 1.0f },            // 0.9 -> 3.2 (engine caps at 4.0)
        .lfoRate           = { 0.15f, 5.907f, 2.0f },                     // oct(0.15, 5.9, E(t,2.0)) -> 9 Hz
        .lfoCyclesPerBeat  = { 0.125f, 5.0f, 2.0f },                      // 1/8 -> 4 cycles/beat
        .lfoAmpDepth       = { 0.0f, 0.35f, 0.08f, 0.85f, 1.0f },         // S(0.08,0.85,t)*0.35
        .lfoFilterDepthOct = { 0.0f, 0.6f, 0.10f, 0.90f, 1.0f },          // S(0.1,0.9,t)*0.6
        .lfoMoveDepth      = { 0.0f, 0.5f, 0.20f, 0.90f, 1.0f },          // S(0.2,0.9,t)*0.5
        .lfoDelayDepthMs   = { 0.0f, 0.9f, 0.40f, 1.00f, 1.0f },          // S(0.4,1,t)*0.9 ms
        .width             = { 1.0f, 2.1f, 0.25f, 0.95f, 1.2f },          // 1 + 1.1*S(0.25,0.95,t)^1.2
        .lowSideCollapse   = { 0.0f, 1.0f, 0.3f, 1.0f, 1.0f },            // 0.8*S(0.3,1,t) (0.8 inside StereoWidth)

        .noiseLevel        = { -66.0f, -16.0f, 2.6f, 0.04f, 0.28f },      // S(0.04,0.28,t) * dB(-66,-16,E(t,2.6))
        .noiseBrightness   = { 0.0f, 1.0f, 0.1f, 0.95f, 1.0f },           // S(0.1,0.95,t)
        .noiseHpf          = { 80.0f, 5.0f, 2.0f },                       // oct(80, 5.0, E(t,2.0))
        .noiseTop          = { 2500.0f, 2.7f, 1.4f },                     // oct(2500, 2.7, P(t,1.4))
        .noiseFiltered     = { 0.0f, 0.30f, 0.1f, 0.95f, 1.0f },          // [design] BP colour

        .reverbSend        = { 0.0f, 1.0f, 0.0f, 0.1f, 1.0f },            // send is full by 10 % (gates what enters)
        .reverbReturn      = { -9.0f, 0.08f, 0.55f, 1.0f },               // S(0.08,0.55,t) * -9 dB
        .reverbRoom        = { 0.35f, 0.92f, 0.1f, 0.9f, 1.0f },          // 0.35 + 0.57*S(0.1,0.9,t)
        .reverbDamping     = { 0.7f, 0.3f, 0.1f, 0.9f, 1.0f },            // 0.7 - 0.4*S(0.1,0.9,t)
        .reverbWidth       = { 0.6f, 1.0f, 0.2f, 0.9f, 1.0f },            // 0.6 + 0.4*S(0.2,0.9,t)
        .reverbPreDelayMs  = { 0.0f, 38.0f, 0.3f, 1.0f, 1.0f },           // 38 ms * S(0.3,1,t)
        .reverbReturnLpf   = { 14000.0f, -1.0f, 0.1f, 0.9f, 1.0f },       // 14 kHz -> 7 kHz
        .reverbSendHpf     = { 120.0f, 0.8745f, 0.1f, 0.9f, 1.0f },       // 120 -> 220 Hz
        .delaySend         = { 0.0f, 1.0f, 0.0f, 0.1f, 1.0f },
        .delayReturn       = { -8.0f, 0.2f, 0.7f, 1.0f },                 // S(0.2,0.7,t) * -8 dB
        .delayFeedback     = { 0.0f, 0.62f, 0.45f, 1.0f, 1.0f },          // 0.62*S(0.45,1,t) (hard cap 0.85 in DelayEngine)
        .delayFbLpf        = { 9000.0f, -1.5f, 0.45f, 1.0f, 1.0f }        // oct(9 kHz, -1.5, S(0.45,1,t)) -> 3.2 kHz
    };

    inline constexpr Profile kAggressive
    {
        .shapes = { LfoShape::ReverseSaw, LfoShape::SampleHold, LfoShape::Triangle, LfoShape::Sine },

        .satDrive          = { 1.0f, 7.0f, 0.10f, 0.95f, 0.98f },         // drive max 7.0, P(S,1.4*0.7)
        .satAmount         = { 0.0f, 0.95f, 0.10f, 0.95f, 0.91f },        // [design] end 0.95
        .hp                = { 20.0f, 5.0f, 2.4f },
        .bpBlend           = { 0.0f, 0.45f, 0.2f, 0.9f, 1.0f },           // [design] stronger spectral movement
        .bpCenter          = { 300.0f, 4.9f, 1.6f },
        .bpQ               = { 0.9f, 3.6f, 0.2f, 0.9f, 1.0f },            // [design]
        .lfoRate           = { 0.15f, 6.737f, 1.4f },                     // max ~16 Hz, k 2.0*0.7
        .lfoCyclesPerBeat  = { 0.125f, 6.0f, 1.4f },                      // [design] 1/8 -> 8 cycles/beat
        .lfoAmpDepth       = { 0.0f, 0.525f, 0.03f, 0.85f, 0.7f },        // 0.35 x 1.5
        .lfoFilterDepthOct = { 0.0f, 0.90f, 0.05f, 0.90f, 0.7f },         // 0.6 x 1.5
        .lfoMoveDepth      = { 0.0f, 0.75f, 0.15f, 0.90f, 0.7f },         // 0.5 x 1.5
        .lfoDelayDepthMs   = { 0.0f, 1.35f, 0.35f, 1.00f, 0.7f },         // 0.9 x 1.5
        .width             = { 1.0f, 2.4f, 0.25f, 0.95f, 1.2f },
        .lowSideCollapse   = { 0.0f, 1.0f, 0.3f, 1.0f, 1.0f },

        .noiseLevel        = { -66.0f, -12.0f, 1.82f, 0.0f, 0.23f },      // ceiling -12, k 2.6*0.7, gate -0.05
        .noiseBrightness   = { 0.0f, 1.0f, 0.05f, 0.95f, 0.7f },
        .noiseHpf          = { 80.0f, 5.0f, 1.4f },
        .noiseTop          = { 2500.0f, 2.7f, 0.98f },
        .noiseFiltered     = { 0.0f, 0.55f, 0.05f, 0.95f, 0.7f },        // [design] white + BP sweep

        .reverbSend        = { 0.0f, 1.0f, 0.0f, 0.1f, 1.0f },
        .reverbReturn      = { -11.0f, 0.12f, 0.55f, 1.0f },              // later onset, -11 dB
        .reverbRoom        = { 0.35f, 0.82f, 0.1f, 0.9f, 1.0f },          // shorter: size 0.82
        .reverbDamping     = { 0.7f, 0.3f, 0.1f, 0.9f, 1.0f },
        .reverbWidth       = { 0.6f, 1.0f, 0.2f, 0.9f, 1.0f },
        .reverbPreDelayMs  = { 0.0f, 38.0f, 0.3f, 1.0f, 1.0f },
        .reverbReturnLpf   = { 14000.0f, -1.0f, 0.1f, 0.9f, 1.0f },
        .reverbSendHpf     = { 120.0f, 0.8745f, 0.1f, 0.9f, 1.0f },
        .delaySend         = { 0.0f, 1.0f, 0.0f, 0.1f, 1.0f },
        .delayReturn       = { -8.0f, 0.2f, 0.7f, 1.0f },
        .delayFeedback     = { 0.0f, 0.72f, 0.45f, 1.0f, 1.0f },
        .delayFbLpf        = { 9000.0f, -1.5f, 0.45f, 1.0f, 1.0f }
    };

    inline const Profile& profileFor (RiseMode mode) noexcept
    {
        switch (mode)
        {
            case RiseMode::SlowBurn:   return kSlowBurn;
            case RiseMode::Aggressive: return kAggressive;
            default:                   return kCinematic;
        }
    }

    // =======================================================================================
    // TensionTargets - the flat destination struct
    // =======================================================================================
    struct TensionTargets
    {
        float engagement = 0.0f;         // e : wet-bus dry/proc crossfade   S(0, 0.02, t)
        float safety     = 0.0f;         // s : OutputSafety knee blend      S(0, 0.03, t)

        // ---- series chain ----
        float inputTrim         = 1.0f;     // linear gain ahead of the pre-filter (identity: the saturator is self-compensated)
        float hpCutoffHz        = 20.0f;    // FilterEngine HP            20 -> 640 Hz
        float bpBlend           = 0.0f;     // FilterEngine BP blend       0 -> 0.30
        float bpCenterHz        = 300.0f;   // FilterEngine BP centre    300 -> ~8.9 kHz
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

        // ---- noise ----
        float noiseGain         = 0.0f;     // linear (RMS-ish), exactly 0 at t = 0
        float noiseBrightness   = 0.0f;     // pink -> white            0 -> 1
        float noiseHpfHz        = 80.0f;    // noise HPF                80 -> 2560 Hz
        float noiseTopHz        = 2500.0f;  // noise LPF / BP centre  2500 -> ~16 kHz
        float noiseFilteredAmt  = 0.0f;     // resonant BP colour        0 -> 0.30

        // ---- reverb / delay ----
        float reverbSend        = 0.0f;     // reverb send gain          0 -> 1 (full by t = 0.1)
        float reverbReturn      = 0.0f;     // reverb return (linear)    0 -> -9 dB, exactly 0 at t = 0
        float reverbRoom        = 0.35f;    //                         0.35 -> 0.92
        float reverbDamping     = 0.7f;     //                          0.7 -> 0.3
        float reverbWidth       = 0.6f;     //                          0.6 -> 1.0
        float reverbPreDelayMs  = 0.0f;     //                            0 -> 38 ms
        float reverbReturnLpfHz = 14000.0f; //                        14000 -> 7000 Hz
        float reverbSendHpfHz   = 120.0f;   //                          120 -> 220 Hz
        float delaySend         = 0.0f;     // delay send gain           0 -> 1 (full by t = 0.1)
        float delayReturn       = 0.0f;     // delay return (linear)     0 -> -8 dB, exactly 0 at t = 0
        float delayFeedback     = 0.0f;     //                           0 -> 0.62 (engine caps at 0.85)
        float delayFbLpfHz      = 9000.0f;  //                        9000 -> 3200 Hz
        float delayLfoDepthMs   = 0.0f;     // lfoB time wobble          0 -> 0.9 ms
    };

    // Every member, in declaration order (used by lerpTargets and by the monotonicity test).
    inline constexpr std::array<float TensionTargets::*, 34> kTargetFields
    {
        &TensionTargets::engagement, &TensionTargets::safety, &TensionTargets::inputTrim,
        &TensionTargets::hpCutoffHz, &TensionTargets::bpBlend, &TensionTargets::bpCenterHz, &TensionTargets::bpQ,
        &TensionTargets::lfoRateHz, &TensionTargets::lfoCyclesPerBeat, &TensionTargets::lfoAmpDepth,
        &TensionTargets::lfoFilterDepthOct, &TensionTargets::lfoMoveDepth,
        &TensionTargets::satDrive, &TensionTargets::satAmount, &TensionTargets::widthFactor, &TensionTargets::lowSideCollapse,
        &TensionTargets::noiseGain, &TensionTargets::noiseBrightness, &TensionTargets::noiseHpfHz,
        &TensionTargets::noiseTopHz, &TensionTargets::noiseFilteredAmt,
        &TensionTargets::reverbSend, &TensionTargets::reverbReturn, &TensionTargets::reverbRoom,
        &TensionTargets::reverbDamping, &TensionTargets::reverbWidth, &TensionTargets::reverbPreDelayMs,
        &TensionTargets::reverbReturnLpfHz, &TensionTargets::reverbSendHpfHz,
        &TensionTargets::delaySend, &TensionTargets::delayReturn, &TensionTargets::delayFeedback,
        &TensionTargets::delayFbLpfHz, &TensionTargets::delayLfoDepthMs
    };

    inline constexpr std::array<const char*, 34> kTargetFieldNames
    {
        "engagement", "safety", "inputTrim",
        "hpCutoffHz", "bpBlend", "bpCenterHz", "bpQ",
        "lfoRateHz", "lfoCyclesPerBeat", "lfoAmpDepth",
        "lfoFilterDepthOct", "lfoMoveDepth",
        "satDrive", "satAmount", "widthFactor", "lowSideCollapse",
        "noiseGain", "noiseBrightness", "noiseHpfHz",
        "noiseTopHz", "noiseFilteredAmt",
        "reverbSend", "reverbReturn", "reverbRoom",
        "reverbDamping", "reverbWidth", "reverbPreDelayMs",
        "reverbReturnLpfHz", "reverbSendHpfHz",
        "delaySend", "delayReturn", "delayFeedback",
        "delayFbLpfHz", "delayLfoDepthMs"
    };

    // A forgotten member would make the struct larger than the list (all members are float).
    static_assert (sizeof (TensionTargets) == kTargetFields.size() * sizeof (float),
                   "kTargetFields must list every TensionTargets member");

    // Destinations that FALL as tension rises (damping / low-pass style); all others are non-decreasing.
    inline constexpr bool isFallingTarget (float TensionTargets::* m) noexcept
    {
        return m == &TensionTargets::reverbDamping
            || m == &TensionTargets::reverbReturnLpfHz
            || m == &TensionTargets::delayFbLpfHz;
    }

    // =======================================================================================
    // Evaluation
    // =======================================================================================
    namespace detail
    {
        inline float evalS (const SCurve& c, float t) noexcept
        {
            return c.start + (c.end - c.start) * curves::power (curves::smooth (c.lo, c.hi, t), c.p);
        }

        inline float evalSOct (const SOct& c, float t) noexcept
        {
            return curves::oct (c.start, c.octaves, curves::power (curves::smooth (c.lo, c.hi, t), c.p));
        }

        inline float evalEOct (const EOct& c, float t) noexcept
        {
            return curves::oct (c.start, c.octaves, curves::expo (t, c.k));
        }

        inline float evalPOct (const POct& c, float t) noexcept
        {
            return curves::oct (c.start, c.octaves, curves::power (t, c.p));
        }

        inline float evalRet (const RetCurve& c, float t) noexcept
        {
            const float shape = curves::power (curves::smooth (c.lo, c.hi, t), c.p);
            return shape > 0.0f ? curves::dbToGain (c.ceilDb) * shape : 0.0f;   // exactly 0 below the onset
        }

        inline float evalNoise (const NoiseLevel& n, float t) noexcept
        {
            const float gate = curves::smooth (n.gateLo, n.gateHi, t);
            if (gate <= 0.0f)
                return 0.0f;                                                   // exactly 0 at t = 0

            return gate * curves::dbLinear (n.floorDb, n.ceilDb, curves::expo (t, n.k));
        }
    }

    // One profile, no blending.
    inline TensionTargets evaluateProfile (const Profile& pr, float t) noexcept
    {
        using namespace detail;
        const float tc = std::clamp (t, 0.0f, 1.0f);

        TensionTargets o;
        o.engagement        = curves::smooth (0.0f, 0.02f, tc);
        o.safety            = curves::smooth (0.0f, 0.03f, tc);

        o.inputTrim         = 1.0f;
        o.hpCutoffHz        = evalEOct (pr.hp, tc);
        o.bpBlend           = evalS (pr.bpBlend, tc);
        o.bpCenterHz        = evalPOct (pr.bpCenter, tc);
        o.bpQ               = evalS (pr.bpQ, tc);
        o.lfoRateHz         = evalEOct (pr.lfoRate, tc);
        o.lfoCyclesPerBeat  = evalEOct (pr.lfoCyclesPerBeat, tc);
        o.lfoAmpDepth       = evalS (pr.lfoAmpDepth, tc);
        o.lfoFilterDepthOct = evalS (pr.lfoFilterDepthOct, tc);
        o.lfoMoveDepth      = evalS (pr.lfoMoveDepth, tc);
        o.satDrive          = evalS (pr.satDrive, tc);
        o.satAmount         = evalS (pr.satAmount, tc);
        o.widthFactor       = evalS (pr.width, tc);
        o.lowSideCollapse   = evalS (pr.lowSideCollapse, tc);

        o.noiseGain         = evalNoise (pr.noiseLevel, tc);
        o.noiseBrightness   = evalS (pr.noiseBrightness, tc);
        o.noiseHpfHz        = evalEOct (pr.noiseHpf, tc);
        o.noiseTopHz        = evalPOct (pr.noiseTop, tc);
        o.noiseFilteredAmt  = evalS (pr.noiseFiltered, tc);

        o.reverbSend        = evalS (pr.reverbSend, tc);
        o.reverbReturn      = evalRet (pr.reverbReturn, tc);
        o.reverbRoom        = evalS (pr.reverbRoom, tc);
        o.reverbDamping     = evalS (pr.reverbDamping, tc);
        o.reverbWidth       = evalS (pr.reverbWidth, tc);
        o.reverbPreDelayMs  = evalS (pr.reverbPreDelayMs, tc);
        o.reverbReturnLpfHz = evalSOct (pr.reverbReturnLpf, tc);
        o.reverbSendHpfHz   = evalSOct (pr.reverbSendHpf, tc);

        o.delaySend         = evalS (pr.delaySend, tc);
        o.delayReturn       = evalRet (pr.delayReturn, tc);
        o.delayFeedback     = evalS (pr.delayFeedback, tc);
        o.delayFbLpfHz      = evalSOct (pr.delayFbLpf, tc);
        o.delayLfoDepthMs   = evalS (pr.lfoDelayDepthMs, tc);
        return o;
    }

    inline TensionTargets evaluateTensionMapping (float t, RiseMode mode) noexcept
    {
        return evaluateProfile (profileFor (mode), t);
    }

    // Member-wise a + (b - a) * w. Equal values stay bit-identical (b - a == 0).
    inline TensionTargets lerpTargets (const TensionTargets& a, const TensionTargets& b, float w) noexcept
    {
        if (w <= 0.0f) return a;
        if (w >= 1.0f) return b;

        TensionTargets o;
        for (auto m : kTargetFields)
            o.*m = a.*m + (b.*m - a.*m) * w;
        return o;
    }

    // =======================================================================================
    // Mode crossfade
    // =======================================================================================
    struct ShapeMix
    {
        std::array<LfoShape, 3> shape  { LfoShape::Sine, LfoShape::Sine, LfoShape::Sine };
        std::array<float, 3>    weight { 1.0f, 0.0f, 0.0f };
        int                     count  = 1;      // entries in use (identical shapes are merged)
    };

    class ModeBlender
    {
    public:
        static constexpr float kBlendSeconds = 0.6f;     // architecture: riseMode crossfade 600 ms

        // Jump to a single mode (first block / reset): no crossfade.
        void snapTo (RiseMode m) noexcept
        {
            weights = { 0.0f, 0.0f, 0.0f };
            weights[(size_t) index (m)] = 1.0f;
            target = m;
        }

        // Start a crossfade towards `m` (no-op if it already is the target).
        void setTarget (RiseMode m) noexcept { target = m; }

        // Advance the weights by `seconds` (linear: a full 0 -> 1 ramp takes kBlendSeconds).
        void step (float seconds) noexcept
        {
            const float ds = seconds / kBlendSeconds;
            const int   c  = index (target);
            float others = 0.0f;

            for (int i = 0; i < 3; ++i)
            {
                if (i == c) continue;
                float w = weights[(size_t) i] - ds;
                if (w < 1.0e-5f) w = 0.0f;          // float residue of the running subtraction: finish exactly
                weights[(size_t) i] = w;
                others += w;
            }

            weights[(size_t) c] = std::min (1.0f, 1.0f - others);    // others == 0 -> exactly 1
        }

        bool     isBlending() const noexcept          { return weights[(size_t) index (target)] < 1.0f; }
        float    getWeight (RiseMode m) const noexcept { return weights[(size_t) index (m)]; }
        RiseMode getTargetMode() const noexcept       { return target; }

        // modeBlend in the architecture's sense: the weight of the NEW (target) profile
        // (0 right after a switch, 1 when settled).
        float    getBlend() const noexcept            { return weights[(size_t) index (target)]; }

        // Weighted profile evaluation: every profile with weight > 0 is evaluated and the whole
        // TensionTargets struct is lerped. Single profile (settled) = plain evaluation.
        TensionTargets evaluate (float t) const noexcept
        {
            TensionTargets acc;
            float total = 0.0f;
            bool  have  = false;

            for (int i = 0; i < 3; ++i)
            {
                const float w = weights[(size_t) i];
                if (w <= 0.0f) continue;

                const auto tg = evaluateTensionMapping (t, fromIndex (i));

                if (! have)
                {
                    acc = tg;
                    total = w;
                    have = true;
                }
                else
                {
                    total += w;
                    acc = lerpTargets (acc, tg, w / total);
                }
            }

            return have ? acc : evaluateTensionMapping (t, RiseMode::Cinematic);
        }

        // Shapes to evaluate (and their normalised weights) for one LFO destination, e.g.
        // shapeMix (&LfoShapes::amp). Identical shapes of different profiles are merged.
        ShapeMix shapeMix (LfoShape LfoShapes::* dest) const noexcept
        {
            ShapeMix m;
            m.count = 0;
            float total = 0.0f;

            for (int i = 0; i < 3; ++i)
            {
                const float w = weights[(size_t) i];
                if (w <= 0.0f) continue;

                const LfoShape s = profileFor (fromIndex (i)).shapes.*dest;
                int k = 0;
                while (k < m.count && m.shape[(size_t) k] != s) ++k;

                if (k == m.count)
                {
                    m.shape[(size_t) k]  = s;
                    m.weight[(size_t) k] = 0.0f;
                    ++m.count;
                }

                m.weight[(size_t) k] += w;
                total += w;
            }

            if (m.count == 0 || total <= 0.0f)
                return ShapeMix {};

            for (int k = 0; k < m.count; ++k)
                m.weight[(size_t) k] /= total;

            return m;
        }

    private:
        static int index (RiseMode m) noexcept { return (int) m; }
        static RiseMode fromIndex (int i) noexcept { return riseModeFromIndex (i); }

        std::array<float, 3> weights { 0.0f, 1.0f, 0.0f };      // SlowBurn, Cinematic, Aggressive
        RiseMode             target = RiseMode::Cinematic;
    };
}
