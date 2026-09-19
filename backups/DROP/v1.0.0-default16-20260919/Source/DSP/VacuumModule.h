#pragma once
#include <juce_dsp/juce_dsp.h>

/**
 * VacuumModule - the signature pre-Impact "collapse" moment.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > VacuumModule
 * and plugins/DROP/.ideas/implementation-spec.md > VACUUMMODULE. Also see
 * architecture.md > Implementation Risks > "VacuumModule Click-Free
 * Collapse" (MEDIUM risk - tight timing budget at extreme tempos).
 *
 * State-driven (NOT IntensityBudget weight-blended, architecture.md
 * Processing Chain "Routing notes") - applies a sample-accurate
 * raised-cosine gain envelope (fade-out -> hold-at-floor -> fade-back-in)
 * multiplicatively to the ENTIRE post-module-chain bus, plus lightweight
 * low/high-content and stereo-width collapse hooks that track the SAME
 * envelope. computeGain() is a pure, stateless function (like
 * TransitionEngine) so click-free-ness (continuous value AND derivative at
 * every segment boundary, including the 200+ BPM edge case where the whole
 * window is very short) is directly unit-testable in isolation.
 *
 * RT-safe: no allocation, only scalar one-pole filter state.
 */
namespace drop
{

class VacuumModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    /**
     * Pure envelope evaluation. `elapsedSamples` is the sample-accurate
     * position within the Vacuum window (may be fractional-precision via
     * double for cross-block-boundary continuity); `totalWindowSamples` is
     * the window's total latched duration (architecture.md: "start/end
     * sample offsets are latched... not recomputed per-block"). Returns 1.0
     * (fully transparent) outside [0, totalWindowSamples).
     */
    static float computeGain(double elapsedSamples, double totalWindowSamples, float depthAmount01) noexcept;

    /**
     * Applies the collapse envelope (gain + low/high-content + stereo-width
     * hooks) to `block` in place. `elapsedSamplesAtBlockStart` must already
     * reflect the sample-accurate position within the window at the START
     * of this call (the caller/segmentation loop guarantees the whole
     * segment stays within the window - see PluginProcessor::processSegment()).
     */
    void process(juce::dsp::AudioBlock<float> block, double elapsedSamplesAtBlockStart,
                 double totalWindowSamples, float depthAmount01) noexcept;

private:
    float lowTrackState[2] = { 0.0f, 0.0f }; // one-pole LP state (per channel) feeding the low/high-content hook
    double sampleRate = 44100.0;
    float crossoverCoeff = 0.0f;

    // architecture.md: "fade-out over the first ~15-25%... held near-floor
    // for the middle portion, then a short ramp back up" - sums to 1.0.
    static constexpr float kFadeOutFraction = 0.2f;
    static constexpr float kHoldFraction = 0.5f;
    static constexpr float kFadeInFraction = 0.3f;

    // architecture.md: "Style-configurable floor, e.g. -18dB to -40dB" -
    // depthAmount01 (driven by `impact`) interpolates within this range.
    static constexpr float kMaxFloorDb = -12.0f;
    static constexpr float kMinFloorDb = -36.0f;

    static constexpr float kCrossoverHz = 300.0f;
};

} // namespace drop
