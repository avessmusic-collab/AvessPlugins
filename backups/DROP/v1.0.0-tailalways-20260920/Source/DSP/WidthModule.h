#pragma once
#include <juce_dsp/juce_dsp.h>

/**
 * WidthModule - Mid/Side stereo width processing with low-band protection.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > WidthModule
 * and plugins/DROP/.ideas/implementation-spec.md > WIDTHMODULE.
 *
 * Splits the derived Side signal into a low band (below ~120Hz, via a
 * complementary one-pole crossover: low + high == side exactly, a
 * deliberately simple RT-safe simplification of a true Linkwitz-Riley
 * crossover, documented here rather than assumed) and a high band. BOTH
 * bands' side gain are scaled toward unity (1.0, fully transparent) as
 * `weight` -> 0 (architecture.md "Always-Process, Weight-Mix" + plan.md's
 * Phase 3.2 "weight scales audible contribution from 0 to full" applies to
 * every module including Width) - but the low band's target gain sits much
 * closer to fully-mono (protected) than the high band's narrow/expand
 * target at any given nonzero weight, satisfying "low band stays protected
 * ... regardless of width weight" without ever fully decoupling low-band
 * protection from the weight-mix pattern.
 *
 * Mono buses have no stereo image to process and are passed through
 * untouched. RT-safe: no allocation, all state is scalar filter/smoothing
 * state (no buffers needed - per-sample recursive processing only).
 *
 * Stage 3 DSP Phase 3.4 addition: `expansionOverride01` (default 0.0,
 * fully backward-compatible with every pre-3.4 call site) lets ImpactEngine
 * drive the HIGH band's target sharply toward a Style-typical expand value
 * for the Impact window (architecture.md ImpactEngine config: "drives
 * WidthModule's side-gain target sharply upward for the Impact... window...
 * same hard-clamp-against-cancellation rule as WidthModule's normal
 * operation"). Deliberately independent of `weight` - it engages even when
 * the shared IntensityBudget has ducked `weight` toward 0 during Impact
 * (architecture.md: "the state machine snaps weights toward ~0... right at
 * Impact"). The LOW band's protection is completely unaffected by this
 * argument (architecture.md: low band protected "regardless of width
 * weight" - this hook only ever touches the high band).
 */
namespace drop
{

class WidthModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    /**
     * Processes `block` (2 channels expected; passes through untouched if
     * fewer). `expansionOverride01` (0-1, default 0) - see class doc comment.
     */
    void process(juce::dsp::AudioBlock<float> block, float weight, float progress, float expansionOverride01 = 0.0f) noexcept;

    // Exposed for the Phase 3.2 unit test harness (low-band mono-sum
    // protection verification).
    static constexpr float kLowBandProtectedGain = 0.15f;

    // architecture.md ImpactEngine config: "Style-configurable target, e.g.
    // 1.3-1.6x" - exposed for the Phase 3.4 unit test harness.
    static constexpr float kExpandTargetGain = 1.5f;

private:
    double sampleRate = 44100.0;
    float crossoverCoeff = 0.0f; // one-pole LP coefficient at kCrossoverHz, recomputed in prepare()
    float lowSideState = 0.0f;   // one-pole LP state for the Side signal (single running filter - Side is already a single derived mono stream)

    juce::SmoothedValue<float> lowGainSmoothed, highGainSmoothed;

    static constexpr float kCrossoverHz = 120.0f;
};

} // namespace drop
