#include "DropStyle.h"
#include <array>

namespace drop
{

float ModuleCurve::evaluate(float progress01) const noexcept
{
    const float t = juce::jlimit(0.0f, 1.0f, progress01);

    switch (shape)
    {
        case ShapeKind::Flat:
            return peakWeight;

        case ShapeKind::Linear:
            return startWeight + (endWeight - startWeight) * t;

        case ShapeKind::EaseInCubic:
        {
            const float e = t * t * t;
            return startWeight + (endWeight - startWeight) * e;
        }

        case ShapeKind::EaseOutCubic:
        {
            const float inv = 1.0f - t;
            const float e = 1.0f - inv * inv * inv;
            return startWeight + (endWeight - startWeight) * e;
        }

        case ShapeKind::Bump:
        {
            if (t < 0.5f)
            {
                const float f = t / 0.5f;
                return startWeight + (peakWeight - startWeight) * f;
            }
            const float f = (t - 0.5f) / 0.5f;
            return peakWeight + (endWeight - peakWeight) * f;
        }

        case ShapeKind::PeakLate:
        {
            constexpr float peakT = 0.85f;
            if (t < peakT)
            {
                const float f = t / peakT;
                const float inv = 1.0f - f;
                const float e = 1.0f - inv * inv;
                return startWeight + (peakWeight - startWeight) * e;
            }
            const float f = (t - peakT) / (1.0f - peakT);
            return peakWeight + (endWeight - peakWeight) * f;
        }
    }

    return startWeight;
}

namespace
{
    constexpr ModuleCurve C(ShapeKind shape, float start, float peak, float end) noexcept
    {
        return ModuleCurve { shape, start, peak, end };
    }

    // architecture.md IntensityBudget config's worked example, per-module
    // base scalars for each SourceProfile, blended toward 1.0 by `strength`
    // (0 = no source-awareness effect at all, 1 = the full base value below,
    // >1 = an exaggerated/more dramatic shift) - lets each Style dial in how
    // strongly source-awareness should reshape its own weight distribution.
    // Index order: Filter, Width, Pitch, Gate, Stutter, Delay, Reverb, Distortion, Digital, Noise.
    std::array<std::array<float, (size_t) BudgetModule::Count>, (size_t) SourceProfile::Count>
    makeSourceAwareTable(float strength) noexcept
    {
        static constexpr float kVocalBase[10]   = { 1.0f, 1.2f, 1.3f, 0.5f, 0.5f, 1.3f, 1.3f, 0.6f, 0.4f, 0.9f };
        static constexpr float kDrumsBase[10]   = { 1.1f, 0.9f, 0.6f, 1.4f, 1.4f, 0.8f, 0.8f, 1.3f, 1.1f, 0.9f };
        static constexpr float kBassBase[10]    = { 0.7f, 0.6f, 1.1f, 0.9f, 0.9f, 0.9f, 0.8f, 0.6f, 0.7f, 0.8f };
        static constexpr float kFullMixBase[10] = { 0.85f, 0.85f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.8f, 0.7f, 0.85f };

        auto blend = [strength](const float* base, std::array<float, 10>& out) noexcept
        {
            for (int i = 0; i < 10; ++i)
                out[(size_t) i] = 1.0f + (base[i] - 1.0f) * strength;
        };

        std::array<std::array<float, 10>, 4> table {};
        blend(kVocalBase,   table[(size_t) SourceProfile::Vocal]);
        blend(kDrumsBase,   table[(size_t) SourceProfile::Drums]);
        blend(kBassBase,    table[(size_t) SourceProfile::Bass]);
        blend(kFullMixBase, table[(size_t) SourceProfile::FullMix]);
        return table;
    }

    // Filter, Width, Pitch, Gate, Stutter, Delay, Reverb, Distortion, Digital, Noise.
    std::array<bool, (size_t) BudgetModule::Count> makeLowFreqFlags(bool filterProtected, bool distortionProtected) noexcept
    {
        return { { filterProtected, true, false, false, false, false, false, distortionProtected, false, false } };
    }

    const std::array<DropStyleData, (size_t) kNumStyles>& styleTable() noexcept
    {
        static const std::array<DropStyleData, (size_t) kNumStyles> table = [] {
            std::array<DropStyleData, (size_t) kNumStyles> t {};

            // ---------------------------------------------------------------
            // 0: Clean EDM - the DEFAULT style (APVTS `style` param default
            // index 0). Timing fields below intentionally reproduce the
            // Phase 3.1-3.4 era's fixed constants EXACTLY (vacuum
            // ThirtySecond/0.025s floor, impact 0.22s, recovery Half) so the
            // pre-existing Phase 3.4 transient-preservation test's
            // precomputed sample offsets (block 760, local offset 184) stay
            // correct without modification - a deliberate, documented
            // supersession-avoidance choice, not a coincidence.
            // ---------------------------------------------------------------
            t[0].name = "Clean EDM";
            t[0].transitionCurveType = CurveType::SCurve;
            t[0].filterMode = FilterMode::LowPass;
            t[0].saturationMode = SaturationMode::Warm;
            t[0].noiseColor = NoiseColor::White;
            t[0].pitchMode = PitchMode::Rise;
            t[0].moduleCurves = { {
                C(ShapeKind::EaseInCubic,  0.05f, 0.0f, 0.75f), // Filter
                C(ShapeKind::EaseInCubic,  0.0f,  0.0f, 0.55f), // Width
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.45f), // Pitch
                C(ShapeKind::PeakLate,     0.0f,  0.75f, 0.4f), // Gate
                C(ShapeKind::PeakLate,     0.0f,  0.6f, 0.3f),  // Stutter
                C(ShapeKind::Linear,       0.1f,  0.0f, 0.5f),  // Delay
                C(ShapeKind::Linear,       0.15f, 0.0f, 0.4f),  // Reverb
                C(ShapeKind::EaseInCubic,  0.0f,  0.0f, 0.35f), // Distortion
                C(ShapeKind::Flat,         0.0f,  0.05f, 0.0f), // Digital
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.3f),  // Noise
            } };
            t[0].budgetEnvelope = C(ShapeKind::EaseInCubic, 0.05f, 0.0f, 0.85f);
            t[0].digitalActivationCurve = C(ShapeKind::EaseInCubic, 0.0f, 0.0f, 0.5f);
            t[0].vacuumDivision = MusicalDivision::ThirtySecond;
            t[0].vacuumFloorSeconds = 0.025;
            t[0].impactWindowSeconds = 0.22;
            t[0].recoveryDivision = MusicalDivision::Half;
            t[0].sourceAwareScalar = makeSourceAwareTable(1.0f);
            t[0].lowFreqProtected = makeLowFreqFlags(true, true);
            t[0].chaosJitterScale = 1.0f;

            // ---------------------------------------------------------------
            // 1: Big Room
            // ---------------------------------------------------------------
            t[1].name = "Big Room";
            t[1].transitionCurveType = CurveType::Exponential;
            t[1].filterMode = FilterMode::Dual;
            t[1].saturationMode = SaturationMode::Warm;
            t[1].noiseColor = NoiseColor::White;
            t[1].pitchMode = PitchMode::Rise;
            t[1].moduleCurves = { {
                C(ShapeKind::EaseInCubic,  0.05f, 0.0f, 0.85f),
                C(ShapeKind::EaseInCubic,  0.05f, 0.0f, 0.7f),
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.5f),
                C(ShapeKind::PeakLate,     0.0f,  0.6f, 0.35f),
                C(ShapeKind::PeakLate,     0.0f,  0.5f, 0.3f),
                C(ShapeKind::Linear,       0.1f,  0.0f, 0.55f),
                C(ShapeKind::Linear,       0.2f,  0.0f, 0.5f),
                C(ShapeKind::EaseInCubic,  0.0f,  0.0f, 0.45f),
                C(ShapeKind::Flat,         0.0f,  0.05f, 0.0f),
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.4f),
            } };
            t[1].budgetEnvelope = C(ShapeKind::EaseInCubic, 0.05f, 0.0f, 0.95f);
            t[1].digitalActivationCurve = C(ShapeKind::EaseInCubic, 0.0f, 0.0f, 0.55f);
            t[1].vacuumDivision = MusicalDivision::Sixteenth;
            t[1].vacuumFloorSeconds = 0.03;
            t[1].impactWindowSeconds = 0.26;
            t[1].recoveryDivision = MusicalDivision::Half;
            t[1].sourceAwareScalar = makeSourceAwareTable(1.0f);
            t[1].lowFreqProtected = makeLowFreqFlags(true, true);
            t[1].chaosJitterScale = 1.1f;

            // ---------------------------------------------------------------
            // 2: Techno Pressure
            // ---------------------------------------------------------------
            t[2].name = "Techno Pressure";
            t[2].transitionCurveType = CurveType::LateAcceleration;
            t[2].filterMode = FilterMode::HighPass;
            t[2].saturationMode = SaturationMode::Driven;
            t[2].noiseColor = NoiseColor::Filtered;
            t[2].pitchMode = PitchMode::Fall;
            t[2].moduleCurves = { {
                C(ShapeKind::EaseOutCubic, 0.1f,  0.0f, 0.6f),
                C(ShapeKind::Linear,       0.0f,  0.0f, 0.4f),
                C(ShapeKind::Linear,       0.0f,  0.0f, 0.2f),
                C(ShapeKind::EaseOutCubic, 0.1f,  0.0f, 0.85f),
                C(ShapeKind::PeakLate,     0.0f,  0.5f, 0.4f),
                C(ShapeKind::PeakLate,     0.0f,  0.55f, 0.35f),
                C(ShapeKind::Flat,         0.0f,  0.15f, 0.0f),
                C(ShapeKind::EaseOutCubic, 0.05f, 0.0f, 0.55f),
                C(ShapeKind::Bump,         0.0f,  0.15f, 0.05f),
                C(ShapeKind::Linear,       0.0f,  0.0f, 0.2f),
            } };
            t[2].budgetEnvelope = C(ShapeKind::EaseOutCubic, 0.1f, 0.0f, 0.8f);
            t[2].digitalActivationCurve = C(ShapeKind::Bump, 0.0f, 0.3f, 0.5f);
            t[2].vacuumDivision = MusicalDivision::ThirtySecond;
            t[2].vacuumFloorSeconds = 0.02;
            t[2].impactWindowSeconds = 0.2;
            t[2].recoveryDivision = MusicalDivision::Quarter;
            t[2].sourceAwareScalar = makeSourceAwareTable(0.9f);
            t[2].lowFreqProtected = makeLowFreqFlags(true, true);
            t[2].chaosJitterScale = 1.0f;

            // ---------------------------------------------------------------
            // 3: Dark Techno
            // ---------------------------------------------------------------
            t[3].name = "Dark Techno";
            t[3].transitionCurveType = CurveType::LateAcceleration;
            t[3].filterMode = FilterMode::BandPass;
            t[3].saturationMode = SaturationMode::Crushed;
            t[3].noiseColor = NoiseColor::Filtered;
            t[3].pitchMode = PitchMode::Fall;
            t[3].moduleCurves = { {
                C(ShapeKind::Bump,        0.1f,  0.7f, 0.3f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.35f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.3f),
                C(ShapeKind::PeakLate,    0.0f,  0.65f, 0.45f),
                C(ShapeKind::PeakLate,    0.0f,  0.6f, 0.5f),
                C(ShapeKind::EaseInCubic, 0.05f, 0.0f, 0.6f),
                C(ShapeKind::Linear,      0.1f,  0.0f, 0.3f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.7f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.6f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.35f),
            } };
            t[3].budgetEnvelope = C(ShapeKind::EaseInCubic, 0.05f, 0.0f, 0.9f);
            t[3].digitalActivationCurve = C(ShapeKind::EaseInCubic, 0.0f, 0.0f, 0.8f);
            t[3].vacuumDivision = MusicalDivision::ThirtySecond;
            t[3].vacuumFloorSeconds = 0.03;
            t[3].impactWindowSeconds = 0.24;
            t[3].recoveryDivision = MusicalDivision::Half;
            t[3].sourceAwareScalar = makeSourceAwareTable(1.1f);
            t[3].lowFreqProtected = makeLowFreqFlags(true, true);
            t[3].chaosJitterScale = 1.3f;

            // ---------------------------------------------------------------
            // 4: Trap Pull
            // ---------------------------------------------------------------
            t[4].name = "Trap Pull";
            t[4].transitionCurveType = CurveType::Exponential;
            t[4].filterMode = FilterMode::LowPass;
            t[4].saturationMode = SaturationMode::Driven;
            t[4].noiseColor = NoiseColor::White;
            t[4].pitchMode = PitchMode::Fall;
            t[4].moduleCurves = { {
                C(ShapeKind::EaseInCubic,  0.05f, 0.0f, 0.6f),
                C(ShapeKind::Linear,       0.0f,  0.0f, 0.45f),
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.7f),
                C(ShapeKind::Bump,         0.0f,  0.5f, 0.2f),
                C(ShapeKind::PeakLate,     0.0f,  0.7f, 0.55f),
                C(ShapeKind::PeakLate,     0.0f,  0.6f, 0.4f),
                C(ShapeKind::Linear,       0.1f,  0.0f, 0.35f),
                C(ShapeKind::Linear,       0.0f,  0.0f, 0.3f),
                C(ShapeKind::Flat,         0.0f,  0.1f, 0.0f),
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.35f),
            } };
            t[4].budgetEnvelope = C(ShapeKind::EaseOutCubic, 0.05f, 0.0f, 0.85f);
            t[4].digitalActivationCurve = C(ShapeKind::Bump, 0.0f, 0.2f, 0.4f);
            t[4].vacuumDivision = MusicalDivision::Sixteenth;
            t[4].vacuumFloorSeconds = 0.03;
            t[4].impactWindowSeconds = 0.25;
            t[4].recoveryDivision = MusicalDivision::Quarter;
            t[4].sourceAwareScalar = makeSourceAwareTable(1.0f);
            t[4].lowFreqProtected = makeLowFreqFlags(true, true);
            t[4].chaosJitterScale = 1.2f;

            // ---------------------------------------------------------------
            // 5: DnB Rush
            // ---------------------------------------------------------------
            t[5].name = "DnB Rush";
            t[5].transitionCurveType = CurveType::Exponential;
            t[5].filterMode = FilterMode::HighPass;
            t[5].saturationMode = SaturationMode::Driven;
            t[5].noiseColor = NoiseColor::Filtered;
            t[5].pitchMode = PitchMode::RiseAndFall;
            t[5].moduleCurves = { {
                C(ShapeKind::EaseOutCubic, 0.1f,  0.0f, 0.7f),
                C(ShapeKind::EaseOutCubic, 0.05f, 0.0f, 0.5f),
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.5f),
                C(ShapeKind::EaseOutCubic, 0.15f, 0.0f, 0.9f),
                C(ShapeKind::EaseOutCubic, 0.1f,  0.0f, 0.75f),
                C(ShapeKind::EaseOutCubic, 0.1f,  0.0f, 0.5f),
                C(ShapeKind::Flat,         0.0f,  0.1f, 0.0f),
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.5f),
                C(ShapeKind::Bump,         0.0f,  0.2f, 0.1f),
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.3f),
            } };
            t[5].budgetEnvelope = C(ShapeKind::EaseOutCubic, 0.1f, 0.0f, 0.9f);
            t[5].digitalActivationCurve = C(ShapeKind::EaseOutCubic, 0.0f, 0.0f, 0.6f);
            t[5].vacuumDivision = MusicalDivision::ThirtySecond;
            t[5].vacuumFloorSeconds = 0.015;
            t[5].impactWindowSeconds = 0.16;
            t[5].recoveryDivision = MusicalDivision::Eighth;
            t[5].sourceAwareScalar = makeSourceAwareTable(0.9f);
            t[5].lowFreqProtected = makeLowFreqFlags(true, true);
            t[5].chaosJitterScale = 1.2f;

            // ---------------------------------------------------------------
            // 6: Hyper
            // ---------------------------------------------------------------
            t[6].name = "Hyper";
            t[6].transitionCurveType = CurveType::Exponential;
            t[6].filterMode = FilterMode::Dual;
            t[6].saturationMode = SaturationMode::Distorted;
            t[6].noiseColor = NoiseColor::White;
            t[6].pitchMode = PitchMode::RiseAndFall;
            t[6].moduleCurves = { {
                C(ShapeKind::Bump, 0.1f, 0.8f, 0.4f),
                C(ShapeKind::Bump, 0.0f, 0.7f, 0.35f),
                C(ShapeKind::Bump, 0.0f, 0.6f, 0.3f),
                C(ShapeKind::Bump, 0.0f, 0.7f, 0.4f),
                C(ShapeKind::Bump, 0.0f, 0.65f, 0.45f),
                C(ShapeKind::Bump, 0.1f, 0.5f, 0.35f),
                C(ShapeKind::Bump, 0.1f, 0.4f, 0.25f),
                C(ShapeKind::Bump, 0.0f, 0.55f, 0.3f),
                C(ShapeKind::Bump, 0.0f, 0.3f, 0.15f),
                C(ShapeKind::Bump, 0.0f, 0.45f, 0.2f),
            } };
            t[6].budgetEnvelope = C(ShapeKind::Bump, 0.1f, 0.6f, 0.9f);
            t[6].digitalActivationCurve = C(ShapeKind::Bump, 0.0f, 0.4f, 0.6f);
            t[6].vacuumDivision = MusicalDivision::Sixteenth;
            t[6].vacuumFloorSeconds = 0.03;
            t[6].impactWindowSeconds = 0.28;
            t[6].recoveryDivision = MusicalDivision::Half;
            t[6].sourceAwareScalar = makeSourceAwareTable(1.0f);
            t[6].lowFreqProtected = makeLowFreqFlags(true, true);
            t[6].chaosJitterScale = 1.4f;

            // ---------------------------------------------------------------
            // 7: Cinematic
            // ---------------------------------------------------------------
            t[7].name = "Cinematic";
            t[7].transitionCurveType = CurveType::SCurve;
            t[7].filterMode = FilterMode::LowPass;
            t[7].saturationMode = SaturationMode::Clean;
            t[7].noiseColor = NoiseColor::Pink;
            t[7].pitchMode = PitchMode::Rise;
            t[7].moduleCurves = { {
                C(ShapeKind::Linear,      0.05f, 0.0f, 0.4f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.5f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.5f),
                C(ShapeKind::Flat,        0.0f,  0.1f, 0.0f),
                C(ShapeKind::Flat,        0.0f,  0.1f, 0.0f),
                C(ShapeKind::EaseInCubic, 0.1f,  0.0f, 0.4f),
                C(ShapeKind::EaseInCubic, 0.15f, 0.0f, 0.65f),
                C(ShapeKind::Flat,        0.0f,  0.05f, 0.0f),
                C(ShapeKind::Flat,        0.0f,  0.02f, 0.0f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.45f),
            } };
            t[7].budgetEnvelope = C(ShapeKind::EaseInCubic, 0.1f, 0.0f, 0.75f);
            t[7].digitalActivationCurve = C(ShapeKind::Flat, 0.0f, 0.05f, 0.0f);
            t[7].vacuumDivision = MusicalDivision::Sixteenth;
            t[7].vacuumFloorSeconds = 0.04;
            t[7].impactWindowSeconds = 0.3;
            t[7].recoveryDivision = MusicalDivision::Whole;
            t[7].sourceAwareScalar = makeSourceAwareTable(0.7f);
            t[7].lowFreqProtected = makeLowFreqFlags(true, true);
            t[7].chaosJitterScale = 0.6f;

            // ---------------------------------------------------------------
            // 8: Minimal
            // ---------------------------------------------------------------
            t[8].name = "Minimal";
            t[8].transitionCurveType = CurveType::Linear;
            t[8].filterMode = FilterMode::LowPass;
            t[8].saturationMode = SaturationMode::Clean;
            t[8].noiseColor = NoiseColor::White;
            t[8].pitchMode = PitchMode::Rise;
            t[8].moduleCurves = { {
                C(ShapeKind::Linear, 0.02f, 0.0f, 0.25f),
                C(ShapeKind::Linear, 0.0f,  0.0f, 0.2f),
                C(ShapeKind::Linear, 0.0f,  0.0f, 0.15f),
                C(ShapeKind::Flat,   0.0f,  0.15f, 0.0f),
                C(ShapeKind::Flat,   0.0f,  0.1f, 0.0f),
                C(ShapeKind::Linear, 0.05f, 0.0f, 0.2f),
                C(ShapeKind::Linear, 0.05f, 0.0f, 0.2f),
                C(ShapeKind::Flat,   0.0f,  0.05f, 0.0f),
                C(ShapeKind::Flat,   0.0f,  0.02f, 0.0f),
                C(ShapeKind::Linear, 0.0f,  0.0f, 0.15f),
            } };
            t[8].budgetEnvelope = C(ShapeKind::Linear, 0.05f, 0.0f, 0.4f);
            t[8].digitalActivationCurve = C(ShapeKind::Flat, 0.0f, 0.03f, 0.0f);
            t[8].vacuumDivision = MusicalDivision::ThirtySecond;
            t[8].vacuumFloorSeconds = 0.02;
            t[8].impactWindowSeconds = 0.15;
            t[8].recoveryDivision = MusicalDivision::Quarter;
            t[8].sourceAwareScalar = makeSourceAwareTable(0.6f);
            t[8].lowFreqProtected = makeLowFreqFlags(true, true);
            t[8].chaosJitterScale = 0.4f;

            // ---------------------------------------------------------------
            // 9: Vocal Build
            // ---------------------------------------------------------------
            t[9].name = "Vocal Build";
            t[9].transitionCurveType = CurveType::SCurve;
            t[9].filterMode = FilterMode::LowPass;
            t[9].saturationMode = SaturationMode::Warm;
            t[9].noiseColor = NoiseColor::Pink;
            t[9].pitchMode = PitchMode::Rise;
            t[9].moduleCurves = { {
                C(ShapeKind::EaseInCubic,  0.05f, 0.0f, 0.35f),
                C(ShapeKind::EaseInCubic,  0.0f,  0.0f, 0.55f),
                C(ShapeKind::EaseOutCubic, 0.0f,  0.0f, 0.75f),
                C(ShapeKind::Flat,         0.0f,  0.1f, 0.0f),
                C(ShapeKind::Flat,         0.0f,  0.08f, 0.0f),
                C(ShapeKind::EaseInCubic,  0.1f,  0.0f, 0.65f),
                C(ShapeKind::EaseInCubic,  0.15f, 0.0f, 0.7f),
                C(ShapeKind::Flat,         0.0f,  0.05f, 0.0f),
                C(ShapeKind::Flat,         0.0f,  0.02f, 0.0f),
                C(ShapeKind::EaseInCubic,  0.0f,  0.0f, 0.3f),
            } };
            t[9].budgetEnvelope = C(ShapeKind::EaseInCubic, 0.05f, 0.0f, 0.7f);
            t[9].digitalActivationCurve = C(ShapeKind::Flat, 0.0f, 0.03f, 0.0f);
            t[9].vacuumDivision = MusicalDivision::Sixteenth;
            t[9].vacuumFloorSeconds = 0.03;
            t[9].impactWindowSeconds = 0.24;
            t[9].recoveryDivision = MusicalDivision::Half;
            t[9].sourceAwareScalar = makeSourceAwareTable(1.2f);
            t[9].lowFreqProtected = makeLowFreqFlags(true, true);
            t[9].chaosJitterScale = 0.8f;

            // ---------------------------------------------------------------
            // 10: Drum Build
            // ---------------------------------------------------------------
            t[10].name = "Drum Build";
            t[10].transitionCurveType = CurveType::LateAcceleration;
            t[10].filterMode = FilterMode::HighPass;
            t[10].saturationMode = SaturationMode::Driven;
            t[10].noiseColor = NoiseColor::White;
            t[10].pitchMode = PitchMode::Rise;
            t[10].moduleCurves = { {
                C(ShapeKind::EaseOutCubic, 0.1f,  0.0f, 0.65f),
                C(ShapeKind::Linear,       0.0f,  0.0f, 0.3f),
                C(ShapeKind::Linear,       0.0f,  0.0f, 0.15f),
                C(ShapeKind::EaseOutCubic, 0.1f,  0.0f, 0.85f),
                C(ShapeKind::EaseOutCubic, 0.05f, 0.0f, 0.8f),
                C(ShapeKind::Linear,       0.05f, 0.0f, 0.3f),
                C(ShapeKind::Flat,         0.0f,  0.1f, 0.0f),
                C(ShapeKind::EaseOutCubic, 0.05f, 0.0f, 0.6f),
                C(ShapeKind::Flat,         0.0f,  0.05f, 0.0f),
                C(ShapeKind::Linear,       0.0f,  0.0f, 0.2f),
            } };
            t[10].budgetEnvelope = C(ShapeKind::EaseOutCubic, 0.1f, 0.0f, 0.85f);
            t[10].digitalActivationCurve = C(ShapeKind::Bump, 0.0f, 0.15f, 0.3f);
            t[10].vacuumDivision = MusicalDivision::ThirtySecond;
            t[10].vacuumFloorSeconds = 0.02;
            t[10].impactWindowSeconds = 0.18;
            t[10].recoveryDivision = MusicalDivision::Quarter;
            t[10].sourceAwareScalar = makeSourceAwareTable(1.2f);
            t[10].lowFreqProtected = makeLowFreqFlags(true, true);
            t[10].chaosJitterScale = 1.0f;

            // ---------------------------------------------------------------
            // 11: Synth Build
            // ---------------------------------------------------------------
            t[11].name = "Synth Build";
            t[11].transitionCurveType = CurveType::Exponential;
            t[11].filterMode = FilterMode::Dual;
            t[11].saturationMode = SaturationMode::Driven;
            t[11].noiseColor = NoiseColor::Filtered;
            t[11].pitchMode = PitchMode::RiseAndFall;
            t[11].moduleCurves = { {
                C(ShapeKind::Bump,        0.05f, 0.6f, 0.4f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.5f),
                C(ShapeKind::Bump,        0.0f,  0.7f, 0.4f),
                C(ShapeKind::Linear,      0.0f,  0.0f, 0.3f),
                C(ShapeKind::Linear,      0.0f,  0.0f, 0.35f),
                C(ShapeKind::EaseInCubic, 0.05f, 0.0f, 0.5f),
                C(ShapeKind::Linear,      0.1f,  0.0f, 0.4f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.4f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.5f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.55f),
            } };
            t[11].budgetEnvelope = C(ShapeKind::EaseInCubic, 0.05f, 0.0f, 0.85f);
            t[11].digitalActivationCurve = C(ShapeKind::EaseInCubic, 0.0f, 0.0f, 0.65f);
            t[11].vacuumDivision = MusicalDivision::Sixteenth;
            t[11].vacuumFloorSeconds = 0.025;
            t[11].impactWindowSeconds = 0.22;
            t[11].recoveryDivision = MusicalDivision::Half;
            t[11].sourceAwareScalar = makeSourceAwareTable(1.0f);
            t[11].lowFreqProtected = makeLowFreqFlags(true, true);
            t[11].chaosJitterScale = 1.1f;

            // ---------------------------------------------------------------
            // 12: Full Mix
            // ---------------------------------------------------------------
            t[12].name = "Full Mix";
            t[12].transitionCurveType = CurveType::SCurve;
            t[12].filterMode = FilterMode::LowPass;
            t[12].saturationMode = SaturationMode::Clean;
            t[12].noiseColor = NoiseColor::White;
            t[12].pitchMode = PitchMode::Rise;
            t[12].moduleCurves = { {
                C(ShapeKind::Linear, 0.05f, 0.0f, 0.3f),
                C(ShapeKind::Linear, 0.0f,  0.0f, 0.25f),
                C(ShapeKind::Linear, 0.0f,  0.0f, 0.2f),
                C(ShapeKind::Flat,   0.0f,  0.2f, 0.0f),
                C(ShapeKind::Flat,   0.0f,  0.15f, 0.0f),
                C(ShapeKind::Linear, 0.05f, 0.0f, 0.25f),
                C(ShapeKind::Linear, 0.05f, 0.0f, 0.3f),
                C(ShapeKind::Flat,   0.0f,  0.1f, 0.0f),
                C(ShapeKind::Flat,   0.0f,  0.03f, 0.0f),
                C(ShapeKind::Linear, 0.0f,  0.0f, 0.2f),
            } };
            t[12].budgetEnvelope = C(ShapeKind::Linear, 0.05f, 0.0f, 0.5f);
            t[12].digitalActivationCurve = C(ShapeKind::Flat, 0.0f, 0.05f, 0.0f);
            t[12].vacuumDivision = MusicalDivision::ThirtySecond;
            t[12].vacuumFloorSeconds = 0.02;
            t[12].impactWindowSeconds = 0.18;
            t[12].recoveryDivision = MusicalDivision::Quarter;
            t[12].sourceAwareScalar = makeSourceAwareTable(0.8f);
            t[12].lowFreqProtected = makeLowFreqFlags(true, true);
            t[12].chaosJitterScale = 0.5f;

            // ---------------------------------------------------------------
            // 13: Destroy - architecture.md's own explicitly-named example
            // Style for relaxing low-frequency filter/distortion protection
            // at very high tension ("allowSubFilter... used sparingly, e.g.
            // only in 'Destroy' style").
            // ---------------------------------------------------------------
            t[13].name = "Destroy";
            t[13].transitionCurveType = CurveType::LateAcceleration;
            t[13].filterMode = FilterMode::BandPass;
            t[13].saturationMode = SaturationMode::Crushed;
            t[13].noiseColor = NoiseColor::Filtered;
            t[13].pitchMode = PitchMode::RiseAndFall;
            t[13].moduleCurves = { {
                C(ShapeKind::EaseInCubic, 0.1f,  0.0f, 0.9f),
                C(ShapeKind::EaseInCubic, 0.05f, 0.0f, 0.85f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.7f),
                C(ShapeKind::PeakLate,    0.0f,  0.85f, 0.6f),
                C(ShapeKind::PeakLate,    0.0f,  0.8f, 0.65f),
                C(ShapeKind::EaseInCubic, 0.1f,  0.0f, 0.75f),
                C(ShapeKind::Linear,      0.15f, 0.0f, 0.4f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.9f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.85f),
                C(ShapeKind::EaseInCubic, 0.0f,  0.0f, 0.6f),
            } };
            t[13].budgetEnvelope = C(ShapeKind::EaseInCubic, 0.1f, 0.0f, 1.0f);
            t[13].digitalActivationCurve = C(ShapeKind::EaseInCubic, 0.0f, 0.0f, 0.95f);
            t[13].vacuumDivision = MusicalDivision::Sixteenth;
            t[13].vacuumFloorSeconds = 0.035;
            t[13].impactWindowSeconds = 0.3;
            t[13].recoveryDivision = MusicalDivision::Half;
            t[13].sourceAwareScalar = makeSourceAwareTable(1.4f);
            t[13].lowFreqProtected = makeLowFreqFlags(false, false); // the one Style that relaxes low-freq Filter/Distortion protection
            t[13].chaosJitterScale = 1.6f;

            // ---------------------------------------------------------------
            // 14: Vacuum - leans into the collapse: a longer Vacuum window
            // and PeakLate Width/Filter/Gate/Stutter curves that peak right
            // before the collapse for maximum contrast.
            // ---------------------------------------------------------------
            t[14].name = "Vacuum";
            t[14].transitionCurveType = CurveType::SCurve;
            t[14].filterMode = FilterMode::LowPass;
            t[14].saturationMode = SaturationMode::Warm;
            t[14].noiseColor = NoiseColor::White;
            t[14].pitchMode = PitchMode::Fall;
            t[14].moduleCurves = { {
                C(ShapeKind::PeakLate, 0.05f, 0.7f, 0.35f),
                C(ShapeKind::PeakLate, 0.0f,  0.65f, 0.3f),
                C(ShapeKind::Linear,   0.0f,  0.0f, 0.3f),
                C(ShapeKind::PeakLate, 0.0f,  0.6f, 0.3f),
                C(ShapeKind::PeakLate, 0.0f,  0.5f, 0.25f),
                C(ShapeKind::Linear,   0.1f,  0.0f, 0.4f),
                C(ShapeKind::Linear,   0.15f, 0.0f, 0.45f),
                C(ShapeKind::Linear,   0.0f,  0.0f, 0.3f),
                C(ShapeKind::Flat,     0.0f,  0.05f, 0.0f),
                C(ShapeKind::EaseOutCubic, 0.0f, 0.0f, 0.3f),
            } };
            t[14].budgetEnvelope = C(ShapeKind::PeakLate, 0.05f, 0.5f, 0.85f);
            t[14].digitalActivationCurve = C(ShapeKind::Flat, 0.0f, 0.05f, 0.0f);
            t[14].vacuumDivision = MusicalDivision::Sixteenth; // longer collapse window than the default ThirtySecond
            t[14].vacuumFloorSeconds = 0.05;
            t[14].impactWindowSeconds = 0.25;
            t[14].recoveryDivision = MusicalDivision::Half;
            t[14].sourceAwareScalar = makeSourceAwareTable(1.0f);
            t[14].lowFreqProtected = makeLowFreqFlags(true, true);
            t[14].chaosJitterScale = 0.9f;

            // ---------------------------------------------------------------
            // 15: Reverse Energy - architecture.md's explicitly-named
            // structural exception: "curves are authored to feel like
            // intensity RECEDES toward the drop and the Impact is
            // comparatively understated with a long, building Recovery" -
            // implemented purely via INVERTED weight curves (start HIGH, end
            // LOW) per architecture.md's own guidance ("no separate code
            // path needed").
            // ---------------------------------------------------------------
            t[15].name = "Reverse Energy";
            t[15].transitionCurveType = CurveType::Exponential;
            t[15].filterMode = FilterMode::HighPass;
            t[15].saturationMode = SaturationMode::Warm;
            t[15].noiseColor = NoiseColor::Pink;
            t[15].pitchMode = PitchMode::Fall;
            t[15].moduleCurves = { {
                C(ShapeKind::Linear, 0.6f,  0.0f, 0.1f),
                C(ShapeKind::Linear, 0.55f, 0.0f, 0.15f),
                C(ShapeKind::Linear, 0.4f,  0.0f, 0.05f),
                C(ShapeKind::Linear, 0.5f,  0.0f, 0.1f),
                C(ShapeKind::Linear, 0.45f, 0.0f, 0.1f),
                C(ShapeKind::Linear, 0.5f,  0.0f, 0.15f),
                C(ShapeKind::Linear, 0.6f,  0.0f, 0.2f),
                C(ShapeKind::Linear, 0.35f, 0.0f, 0.05f),
                C(ShapeKind::Linear, 0.15f, 0.0f, 0.02f),
                C(ShapeKind::Linear, 0.4f,  0.0f, 0.1f),
            } };
            t[15].budgetEnvelope = C(ShapeKind::Linear, 0.7f, 0.0f, 0.25f); // recedes toward the drop - understated Impact by design
            t[15].digitalActivationCurve = C(ShapeKind::Flat, 0.0f, 0.1f, 0.0f);
            t[15].vacuumDivision = MusicalDivision::ThirtySecond;
            t[15].vacuumFloorSeconds = 0.03;
            t[15].impactWindowSeconds = 0.3;
            t[15].recoveryDivision = MusicalDivision::Whole; // "a long, building Recovery"
            t[15].sourceAwareScalar = makeSourceAwareTable(0.9f);
            t[15].lowFreqProtected = makeLowFreqFlags(true, true);
            t[15].chaosJitterScale = 1.0f;

            return t;
        }();

        return table;
    }
}

const DropStyleData& getStyle(int styleIndex) noexcept
{
    const auto& table = styleTable();
    const int idx = juce::jlimit(0, kNumStyles - 1, styleIndex);
    return table[(size_t) idx];
}

} // namespace drop
