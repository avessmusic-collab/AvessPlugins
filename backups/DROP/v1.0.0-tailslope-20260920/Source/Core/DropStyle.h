#pragma once
#include <array>
#include <juce_core/juce_core.h>
#include "../DSP/IntensityBudget.h"     // BudgetModule enum
#include "../DSP/AudioAnalyzer.h"       // SourceProfile enum
#include "../DSP/TransitionEngine.h"    // CurveType
#include "../DSP/MusicalClock.h"        // MusicalDivision
#include "../DSP/FilterModule.h"        // FilterMode
#include "../DSP/SaturationModule.h"    // SaturationMode
#include "../DSP/NoiseModule.h"         // NoiseColor
#include "../DSP/PitchModule.h"         // PitchMode

/**
 * DropStyle - the data-driven "authored personality" layer behind all 16
 * factory Drop Styles.
 *
 * See plugins/DROP/.ideas/architecture.md > System Architecture >
 * Data-Driven Style System > DropStyle, and
 * plugins/DROP/.ideas/implementation-spec.md > DROPSTYLE ("Do not hard-code
 * large switch statements throughout DSP").
 *
 * Each DropStyleData instance is pure DATA (a `static const` table entry, no
 * per-Style code path) consumed by:
 *   - IntensityBudget::computeWeights() (moduleCurves, budgetEnvelope,
 *     digitalActivationCurve, sourceAwareScalar, chaosJitterScale),
 *   - PluginProcessor (transitionCurveType -> TransitionEngine::setCurveType(),
 *     filterMode/saturationMode/noiseColor/pitchMode -> the corresponding
 *     module's setMode()/setColor() at Arming, vacuumDivision/
 *     vacuumFloorSeconds/impactWindowSeconds/recoveryDivision ->
 *     computeDurationsForNewBuild()).
 *
 * Per-module weight-over-progress curves are authored as a small PARAMETRIC
 * shape (ShapeKind + 3 floats) rather than literal breakpoint-table data -
 * architecture.md explicitly allows this ("an authored function/lookup, e.g.
 * sampled at 9-17 control points... OR an authored function"). This keeps
 * the 16-style x 10-module (=160) curve table tractable to author by hand
 * while still producing genuinely different SHAPES (not just different
 * constants) across styles/modules - see ModuleCurve::evaluate()'s 6 shape
 * kinds. All authoring lives in DropStyle.cpp (a data file); no DSP module
 * contains a per-Style switch statement.
 */
namespace drop
{

constexpr int kNumStyles = 16;

enum class ShapeKind
{
    Flat = 0,      // constant == peakWeight
    Linear,        // lerp(start, end, t)
    EaseInCubic,   // slow start, accelerating rise/fall toward end (t^3 easing)
    EaseOutCubic,  // fast start, decelerating toward end (1-(1-t)^3 easing)
    Bump,          // rises start->peak across [0,0.5], then peak->end across [0.5,1]
    PeakLate       // rises start->peak (eased) across [0,0.85], then peak->end across [0.85,1]
};

struct ModuleCurve
{
    ShapeKind shape = ShapeKind::Linear;
    float startWeight = 0.0f;
    float peakWeight = 0.5f;
    float endWeight = 0.5f;

    /** Pure, stateless, RT-safe. `progress01` clamped to [0,1] internally. */
    float evaluate(float progress01) const noexcept;
};

struct DropStyleData
{
    const char* name = "";

    CurveType transitionCurveType = CurveType::Linear;
    FilterMode filterMode = FilterMode::LowPass;
    SaturationMode saturationMode = SaturationMode::Warm;
    NoiseColor noiseColor = NoiseColor::White;
    PitchMode pitchMode = PitchMode::Rise;

    // Per-module base weight(progress) curve, index-matched to BudgetModule
    // (Filter, Width, Pitch, Gate, Stutter, Delay, Reverb, Distortion,
    // Digital, Noise - the PINNED order, see IntensityBudget.h's
    // BudgetModule doc comment).
    std::array<ModuleCurve, (size_t) BudgetModule::Count> moduleCurves {};

    // totalBudget(progress) ceiling, as a 0-1 fraction of the full
    // BudgetModule::Count-module pool (architecture.md IntensityBudget
    // config: "near-zero at Idle/early Building, approaching 1.0 near
    // Peak... shaped by tension" - tension's own influence is folded into
    // this curve's authored shape per Style, not a separate runtime input).
    ModuleCurve budgetEnvelope {};

    // Digital's tension/chaos-gated activation curve (architecture.md
    // DigitalModule config: "Style-authored activation curve is
    // intentionally near-zero for most of the build and only rises sharply
    // in the final tension-heavy portion and/or scales with chaos") -
    // evaluated at a blended tension/chaos value, NOT progress - see
    // IntensityBudget::computeWeights().
    ModuleCurve digitalActivationCurve {};

    // Timing preferences (architecture.md: "Vacuum duration/floor, Impact
    // tail length, Recovery length are all Style-configurable").
    MusicalDivision vacuumDivision = MusicalDivision::ThirtySecond;
    double vacuumFloorSeconds = 0.025;
    double impactWindowSeconds = 0.22;

    // POST-1.0 (2026-09-17, user-directed): scales ImpactEngine's
    // SYNTHESIZED accents only - the 60Hz sub-hit ("kick") and 2.2kHz noise
    // burst ("snap") that are additively layered on the drop. 0.0 = the
    // impact stays purely the source's own clean return (creative-brief.md:
    // the sub/noise accents are explicitly "optional"). Does NOT affect the
    // source-transient emphasis, width expansion, short-tail reverb on the
    // bus, or the Vacuum - those remain governed by the `impact` parameter.
    //
    // FACTORY-WIDE DEFAULT 0.0 (same day, second user report: "remove
    // impacts" - the first report disabled them for Dark Techno only, this
    // one removes them everywhere): NO factory Style layers synthesized
    // hits any more; every drop is the source's own clean return. The
    // mechanism (and ImpactEngine's generators) are kept intact and
    // per-Style overridable should accents ever be wanted back.
    float impactAccentLevel = 0.0f;
    // POST-1.0 OVERRIDE NOTE (2026-09-16, user-directed improvement, see
    // plugins/DROP/CHANGELOG.md): this field is now VESTIGIAL for Recovery's
    // actual duration - PluginProcessor::computeDurationsForNewBuild() no
    // longer reads it there, having been superseded by the new `tail` APVTS
    // parameter's exponential seconds mapping (Recovery IS the tail's
    // ring-out window - see PluginProcessor.h's "POST-1.0 IMPROVEMENT" doc
    // comment). Left in place (not removed) purely for authoring-data
    // completeness/documentation of each Style's ORIGINAL recommended
    // recovery-length intent; Vacuum/Impact timing above remain fully
    // Style-driven and unaffected by this override.
    MusicalDivision recoveryDivision = MusicalDivision::Half;

    // Source-awareness: per-SourceProfile, per-module scalar multiplier,
    // applied BEFORE the budget cap (architecture.md IntensityBudget
    // config's worked example: "vocal source scales Pitch/Delay/Reverb/
    // Width up ~1.2x and Gate/Stutter/Digital down ~0.5x, drum source does
    // the inverse, bass/full-mix apply stronger low-frequency-protection
    // scalars to Width/Filter/Distortion"). Indexed [SourceProfile][BudgetModule].
    std::array<std::array<float, (size_t) BudgetModule::Count>, (size_t) SourceProfile::Count> sourceAwareScalar {};

    // Low-frequency-protection flags (documented per-module metadata -
    // architecture.md: "low-frequency-protection flags/thresholds... the
    // effective floor is enforced... except when [a Style] flag is set" -
    // e.g. Destroy is the one Style that relaxes Filter/Distortion
    // protection at very high tension, per architecture.md's own example).
    std::array<bool, (size_t) BudgetModule::Count> lowFreqProtected {};

    // Chaos jitter-range multiplier (architecture.md: "jitterRange authored
    // per-Style").
    float chaosJitterScale = 1.0f;
};

/** Clamped 0..kNumStyles-1. `static const` table, no allocation, RT-safe. */
const DropStyleData& getStyle(int styleIndex) noexcept;

} // namespace drop
