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

float IntensityBudget::computeNoiseWeight(const DropStyleData& style, float noiseMacro01, float intensity01,
                                           SourceProfile sourceProfile, float noiseChaosJitter01) noexcept
{
    // See this method's doc comment in IntensityBudget.h for the full
    // root-cause/fix rationale (POST-1.0 FIFTH IMPROVEMENT SESSION,
    // 2026-09-17). Deliberately does NOT take a `progress` parameter -
    // that is the entire point: progression lives solely in
    // NoiseModule::riseAmountForProgress() from here on.
    constexpr size_t noiseIdx = (size_t) BudgetModule::Noise;

    const float macro = juce::jlimit(0.0f, 1.0f, noiseMacro01);
    const float intensity = juce::jlimit(0.0f, 1.0f, intensity01);

    // Style's own Noise ModuleCurve sampled ONCE at its END (progress==1.0)
    // - every Style's Noise curve shape (EaseOutCubic/Linear/EaseInCubic -
    // see DropStyle.cpp, none use Flat for Noise) evaluates to exactly its
    // authored `endWeight` at t==1.0, so this is simply "this Style's
    // ceiling for the noise riser", with no live progress-dependence.
    const float styleNoiseEnd = juce::jlimit(0.0f, 1.0f, style.moduleCurves[noiseIdx].evaluate(1.0f));

    const size_t sourceIndex = (size_t) juce::jlimit(0, (int) SourceProfile::Count - 1, (int) sourceProfile);
    const float sourceScalar = juce::jmax(0.0f, style.sourceAwareScalar[sourceIndex][noiseIdx]);

    float w = macro * intensity * styleNoiseEnd * sourceScalar;

    // Chaos jitter - same additive convention as computeWeights() above,
    // scaled by this Style's own chaosJitterScale and by styleNoiseEnd (a
    // Style with a near-zero Noise ceiling isn't jittered into unmusical
    // prominence). No progress term anywhere in this function.
    w += noiseChaosJitter01 * style.chaosJitterScale * styleNoiseEnd;

    return juce::jlimit(0.0f, 1.0f, w);
}

} // namespace drop
