#include "IntensityBudget.h"

namespace drop
{

IntensityBudget::Weights IntensityBudget::compute(const std::array<float, (size_t) BudgetModule::Count>& macroPercent01,
                                                    float progress, float intensity01) const noexcept
{
    Weights result;

    const float p = juce::jlimit(0.0f, 1.0f, progress);
    const float intensity = juce::jlimit(0.0f, 1.0f, intensity01);

    constexpr size_t N = (size_t) BudgetModule::Count;
    std::array<float, N> raw {};
    float sum = 0.0f;

    for (size_t i = 0; i < N; ++i)
    {
        const float macro = juce::jlimit(0.0f, 1.0f, macroPercent01[i]);
        // Placeholder per-module base weight curve: identical, fixed,
        // rising-with-progress shape for every module (real, distinct,
        // DropStyle-authored per-module curves land in Phase 3.5).
        raw[i] = macro * p;
        sum += raw[i];
    }

    // Placeholder total budget envelope: rises with progress
    // (architecture.md: "near-zero at Idle/early Building, approaching 1.0
    // near Peak") and scales with `intensity`. kBudgetHeadroom keeps the cap
    // from trivially dominating at moderate settings while still
    // demonstrably engaging when several macros sit near 100% simultaneously
    // at once - the literal "do not let every effect reach maximum
    // simultaneously" mechanism, testable via the sum(output) <= totalBudget
    // invariant.
    constexpr float kBudgetHeadroom = 0.6f;
    const float totalBudget = intensity * p * (float) N * kBudgetHeadroom;

    float scale = 1.0f;
    if (sum > totalBudget && sum > 1.0e-6f)
        scale = totalBudget / sum;

    for (size_t i = 0; i < N; ++i)
        result.value[i] = juce::jlimit(0.0f, 1.0f, raw[i] * scale);

    return result;
}

} // namespace drop
