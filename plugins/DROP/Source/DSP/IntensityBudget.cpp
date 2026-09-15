#include "IntensityBudget.h"
#include "../Core/DropStyle.h"

namespace drop
{

float IntensityBudget::totalBudgetFor(const DropStyleData& style, float progress, float intensity01) noexcept
{
    const float p = juce::jlimit(0.0f, 1.0f, progress);
    const float intensity = juce::jlimit(0.0f, 1.0f, intensity01);
    const float ceiling01 = juce::jlimit(0.0f, 1.0f, style.budgetEnvelope.evaluate(p));
    constexpr float N = (float) (size_t) BudgetModule::Count;
    return intensity * ceiling01 * N;
}

IntensityBudget::Weights IntensityBudget::computeWeights(const DropStyleData& style, float progress,
    const std::array<float, (size_t) BudgetModule::Count>& macroPercent01,
    float intensity01, float tension01, float chaos01,
    SourceProfile sourceProfile,
    const std::array<float, (size_t) BudgetModule::Count>& chaosJitter01) noexcept
{
    Weights result;
    constexpr size_t N = (size_t) BudgetModule::Count;

    const float p = juce::jlimit(0.0f, 1.0f, progress);
    const float tension = juce::jlimit(0.0f, 1.0f, tension01);
    const float chaos = juce::jlimit(0.0f, 1.0f, chaos01);

    const size_t sourceIndex = juce::jlimit(0, (int) SourceProfile::Count - 1, (int) sourceProfile);
    const auto& scalars = style.sourceAwareScalar[sourceIndex];

    // Digital's activation curve (architecture.md DigitalModule config:
    // "gated primarily by tension/chaos rather than raw progress") -
    // evaluated at a blended tension/chaos input, NOT at `progress`, unlike
    // every other module's curve.
    const float digitalGate = juce::jlimit(0.0f, 1.0f,
        style.digitalActivationCurve.evaluate(juce::jlimit(0.0f, 1.0f, 0.5f * tension + 0.5f * chaos)));

    std::array<float, N> raw {};
    float sum = 0.0f;

    for (size_t i = 0; i < N; ++i)
    {
        const float macro = juce::jlimit(0.0f, 1.0f, macroPercent01[i]);
        const float styleWeight = juce::jlimit(0.0f, 1.0f, style.moduleCurves[i].evaluate(p));

        float w = styleWeight * macro;

        // Source-aware secondary scalar (architecture.md IntensityBudget
        // config: "applies a secondary per-module scalar... before the
        // budget cap is applied, so source-awareness never breaks the 1.0
        // ceiling").
        w *= juce::jmax(0.0f, scalars[i]);

        // Digital-only tension/chaos activation gate (see doc comment above).
        if (i == (size_t) BudgetModule::Digital)
            w *= digitalGate;

        // Chaos jitter (architecture.md: "bounded jitter... using the
        // deterministic RNG stream... applied once per Arming so a single
        // build's weight curve stays smooth and reproducible") - scaled
        // proportionally to the module's own style-authored weight (a
        // near-zero-weight module is not jittered into unmusical
        // prominence) and by this Style's own authored jitter range.
        w += chaosJitter01[i] * style.chaosJitterScale * styleWeight;

        raw[i] = juce::jmax(0.0f, w);
        sum += raw[i];
    }

    const float totalBudget = totalBudgetFor(style, p, intensity01);

    // Direct weighted-and-capped proportional rescaling (architecture.md
    // Architecture Decisions: "if sum > budget: scale all down
    // proportionally by budget/sum" - NOT softmax). Unchanged from the
    // Phase 3.2 placeholder's already-final normalization logic.
    float scale = 1.0f;
    if (sum > totalBudget && sum > 1.0e-6f)
        scale = totalBudget / sum;

    for (size_t i = 0; i < N; ++i)
        result.value[i] = juce::jlimit(0.0f, 1.0f, raw[i] * scale);

    return result;
}

} // namespace drop
