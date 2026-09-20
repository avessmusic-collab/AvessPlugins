#pragma once
#include <juce_dsp/juce_dsp.h>

/**
 * DigitalModule - bit-depth + sample-rate reduction "controlled accent".
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > DigitalModule
 * and plugins/DROP/.ideas/implementation-spec.md > DIGITALMODULE.
 *
 * Two independent, deliberately-separated controls, matching architecture.md
 * exactly:
 *   1. `severity` (internal, NOT the module's `weight` argument) - how
 *      aggressively the signal is crushed (bit-depth floor / SR-reduction
 *      floor), driven by progress*tension*chaos so it is "intentionally
 *      near-zero for most of the build and only rises sharply in the final
 *      tension-heavy portion and/or scales with chaos" (architecture.md).
 *   2. `weight` (the module's IntensityBudget macro weight, exactly like
 *      every other module) - the standard dry/wet blend, so the module is
 *      still exactly transparent at weight == 0 regardless of severity.
 *
 * DC-blocked (one-pole HP ~5Hz) since hard bit-crushing can introduce DC
 * offset on asymmetric input (architecture.md). Always processes
 * (Always-Process, Weight-Mix); RT-safe, no allocation in process().
 */
namespace drop
{

class DigitalModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    /**
     * `tension01`/`chaos01` are the `tension`/`chaos` APVTS parameters,
     * normalized 0-1 (architecture.md: "gated primarily by tension/chaos
     * rather than raw progress").
     */
    void process(juce::dsp::AudioBlock<float> block, float weight, float progress, float tension01, float chaos01) noexcept;

private:
    juce::dsp::IIR::Filter<float> dcBlockers[2];
    juce::SmoothedValue<float> weightSmoothed;
    double sampleRate = 44100.0;

    float heldSample[2] = { 0.0f, 0.0f };
    double srCounterPhase[2] = { 0.0, 0.0 };

    static constexpr float kMinBitDepth = 4.0f;
    static constexpr float kMaxBitDepth = 16.0f; // near-transparent at severity == 0
    static constexpr float kMinSrRatio = 1.0f / 8.0f; // effective-rate floor: 1/8 native
    static constexpr double kDcBlockerHz = 5.0;
};

} // namespace drop
