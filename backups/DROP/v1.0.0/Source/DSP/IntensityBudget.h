#pragma once
#include <array>
#include <juce_core/juce_core.h>
#include "AudioAnalyzer.h"

/**
 * IntensityBudget - REAL, Style-driven implementation (Stage 3 DSP Phase 3.5
 * scope). Replaces the Phase 3.2 placeholder's fixed/static per-module base
 * curve with DropStyle-authored weight curves, and adds source-aware
 * adjustment + Chaos jitter, both applied BEFORE the proportional-cap
 * normalization step (the normalization/capping logic itself is unchanged
 * from Phase 3.2 - already final per architecture.md's "Architecture
 * Decisions > IntensityBudget Normalization: Proportional Cap vs. Softmax").
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > IntensityBudget
 * and plugins/DROP/.ideas/implementation-spec.md > INTENSITYBUDGET.
 *
 * Pure, stateless, unit-testable function (architecture.md's IntensityBudget
 * mitigation strategy: "implement IntensityBudget with a unit-testable pure
 * function (weights = computeWeights(style, progress, macros, intensity,
 * tension, sourceProfile, chaosJitter))") - `computeWeights()` is `static`
 * (no instance state whatsoever). No allocation, no JUCE dependency beyond
 * jlimit/jmax - RT-safe by construction.
 *
 * `impact` is DELIBERATELY excluded from this pool (architecture.md
 * Architecture Decisions / Parameter Interactions: "impact is deliberately
 * NOT part of the shared 1.0 IntensityBudget pool... raising impact never
 * starves Filter/Width/Pitch/etc. of budget, and lowering those macros never
 * boosts impact's headroom") - there is no `impact` entry in BudgetModule and
 * no `impact` parameter anywhere in this class's API; this is a structural
 * guarantee, not a runtime check.
 */
namespace drop
{

// Matches architecture.md's 10 IntensityBudget-managed modules, in the
// PINNED order used throughout the codebase (PluginProcessor's macro-param
// wiring, the visualization ring buffer's activeModuleWeights[10] array, and
// DropStyle's per-module curve tables all share this exact index order -
// see PluginProcessor.h's VisualizationFrame doc comment).
enum class BudgetModule
{
    Filter = 0,
    Width,
    Pitch,
    Gate,
    Stutter,
    Delay,
    Reverb,
    Distortion,
    Digital,
    Noise,
    Count
};

// Forward declaration only (full definition: Source/Core/DropStyle.h).
// Deliberately NOT #included here - DropStyle.h itself #includes THIS header
// for the BudgetModule enum, so a full two-way include would be circular.
// computeWeights() only ever needs a `const DropStyleData&`, which a forward
// declaration is sufficient for; IntensityBudget.cpp #includes the full
// definition.
struct DropStyleData;

class IntensityBudget
{
public:
    struct Weights
    {
        std::array<float, (size_t) BudgetModule::Count> value {};
    };

    /**
     * `style` - the currently-active (Arming-latched) DropStyle, supplying
     * per-module base weight curves + the budget-envelope ceiling curve +
     * source-awareness scalars + the Digital activation curve + the Chaos
     * jitter-range scale.
     * `progress` - the current transition progress / budget-envelope input
     * (0-1; see DROPAudioProcessor::computeIntensityProgress()).
     * `macroPercent01[i]` - module `i`'s macro parameter, normalized 0-1.
     * `intensity01`/`tension01`/`chaos01` - the `intensity`/`tension`/`chaos`
     * APVTS parameters, normalized 0-1.
     * `sourceProfile` - AudioAnalyzer's current classification, driving the
     * Style's per-SourceProfile scalar table.
     * `chaosJitter01[i]` - module `i`'s bounded +-jitter fraction, already
     * scaled by `chaos01` (see DropRandomizer::getIntensityJitter()) - this
     * function further scales it by `style.chaosJitterScale` and by the
     * module's own style-authored weight (so a near-zero-weight module isn't
     * jittered into unmusical prominence).
     *
     * Invariant (unit-tested): sum(result.value) <= totalBudgetFor(style,
     * progress, intensity01) (up to floating-point tolerance) - the literal
     * mechanism implementing "Do not let every effect reach maximum
     * simultaneously."
     */
    static Weights computeWeights(const DropStyleData& style,
                                   float progress,
                                   const std::array<float, (size_t) BudgetModule::Count>& macroPercent01,
                                   float intensity01,
                                   float tension01,
                                   float chaos01,
                                   SourceProfile sourceProfile,
                                   const std::array<float, (size_t) BudgetModule::Count>& chaosJitter01) noexcept;

    /**
     * `totalBudget(progress) = intensity01 * style.budgetEnvelope(progress)
     * * BudgetModule::Count` (architecture.md's exact formula). Exposed
     * publicly so the sum<=totalBudget invariant is directly testable
     * without duplicating this arithmetic in the test harness.
     */
    static float totalBudgetFor(const DropStyleData& style, float progress, float intensity01) noexcept;
};

} // namespace drop
