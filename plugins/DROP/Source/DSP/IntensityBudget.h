#pragma once
#include <array>
#include <juce_core/juce_core.h>

/**
 * IntensityBudget - PLACEHOLDER implementation (Stage 3 DSP Phase 3.2 scope).
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > IntensityBudget
 * and plugins/DROP/.ideas/implementation-spec.md > INTENSITYBUDGET.
 *
 * This is EXPLICITLY a placeholder per plan.md Phase 3.2: "A minimal
 * placeholder IntensityBudget (fixed/static weights, not yet Style-driven)
 * sufficient to exercise the always-process/weight-mix pattern across
 * [the] six modules." It implements the real, final NORMALIZATION algorithm
 * (proportional-cap: architecture.md's "direct weighted-and-capped
 * proportional rescaling," NOT softmax) with a fixed, identical per-module
 * base weight curve instead of DropStyle-authored per-module curves -
 * source-aware adjustment, Chaos jitter, and real Style curves all land in
 * Phase 3.5, which will replace only the `raw[i]` computation below, not the
 * normalization/capping logic (already final).
 *
 * Pure, stateless, unit-testable function (architecture.md's mitigation
 * strategy: "implement IntensityBudget with a unit-testable pure function").
 * No allocation, no JUCE dependency beyond jlimit/jmap - RT-safe by
 * construction.
 */
namespace drop
{

// Matches architecture.md's 10 IntensityBudget-managed modules. Phase 3.2
// only wires Filter/Width/Gate/Delay/Reverb/Distortion into real DSP;
// Pitch/Stutter/Digital/Noise are budget-tracked here for forward
// compatibility but not yet consumed by any DSP module (Phase 3.3).
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

class IntensityBudget
{
public:
    struct Weights
    {
        std::array<float, (size_t) BudgetModule::Count> value {};
    };

    /**
     * `macroPercent01[i]` - the module's macro parameter, normalized 0-1
     * (already divided by 100 by the caller). `progress` - the current
     * transition progress / budget-envelope input (0-1; see
     * DROPAudioProcessor::computeIntensityProgress() for how the caller
     * derives this per DropStateMachine state). `intensity01` - the
     * `intensity` APVTS parameter, normalized 0-1.
     *
     * Invariant (unit-tested): sum(result.value) <= totalBudget(progress)
     * (up to floating-point tolerance) - the literal mechanism implementing
     * "Do not let every effect reach maximum simultaneously."
     */
    Weights compute(const std::array<float, (size_t) BudgetModule::Count>& macroPercent01,
                     float progress, float intensity01) const noexcept;
};

} // namespace drop
