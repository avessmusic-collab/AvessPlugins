#pragma once
#include <juce_dsp/juce_dsp.h>
#include "MusicalClock.h" // MusicalDivision (Stage 3 DSP Phase 3.5: setChaosDivision() needs the full enum, not just a forward-declared class)

/**
 * DelayModule - tempo-synced stereo delay with a feedback-safe path.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > DelayModule
 * and plugins/DROP/.ideas/implementation-spec.md > DELAYMODULE.
 *
 * Stage 3 DSP Phase 3.2 scope: fixed 1/8-note division time (Style-driven
 * musical-division choice lands in Phase 3.5), right-channel stereo offset
 * (1.0625x, "dotted-offset trick" per architecture.md), feedback HARD
 * double-guarded against runaway: tanh soft-clip (bounds to [-1,1]) THEN
 * multiplied by a coefficient hard-clamped to <= kMaxFeedback (0.92) - the
 * recursive loop gain can therefore never reach or exceed 1.0 regardless of
 * how `weight`/`progress` are automated.
 *
 * juce::dsp::DelayLine<float, Lagrange3rd> for click-free fractional-delay
 * interpolation. Delay line is sized in prepare() for the longest supported
 * division (1/1 note) at the lowest supported tempo (40 BPM) at 192kHz -
 * never resized in process(). Always processes (never bypassed) per
 * architecture.md's "Always-Process, Weight-Mix" pattern - a delay line
 * that's always running has no cold-start transient when weight rises.
 */
namespace drop
{

class MusicalClock;

class DelayModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    void process(juce::dsp::AudioBlock<float> block, float weight, float progress, const MusicalClock& clock) noexcept;

    /**
     * Stage 3 DSP Phase 3.5: the base (left-channel) delay division is
     * quantized toward this Chaos-drawn division instead of the fixed
     * Eighth-note placeholder (architecture.md Integration Points:
     * "DropRandomizer (Chaos) -> ... DelayModule division" - latched once
     * per Arming via DropRandomizer::getDelayDivisionChoice()). Defaults to
     * Eighth (identical to the pre-3.5 fixed behavior) until called.
     */
    void setChaosDivision(MusicalDivision division) noexcept { chaosDivision = division; }

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> delayLine;
    juce::dsp::IIR::Filter<float> feedbackHpf[2], feedbackLpf[2];
    juce::AudioBuffer<float> dryBuffer;
    juce::SmoothedValue<float> weightSmoothed;

    double sampleRate = 44100.0;
    double maxDelaySamples = 0.0;
    MusicalDivision chaosDivision = MusicalDivision::Eighth;

    // architecture.md: "Feedback clamped hard to <=0.92 and additionally
    // soft-saturated before being written back into the delay line."
    static constexpr float kMaxFeedback = 0.92f;
    static constexpr float kFeedbackDrive = 1.2f;
    static constexpr double kFeedbackHpfHz = 80.0;  // protects low end from delay-induced buildup
    static constexpr double kFeedbackLpfHz = 6000.0; // darkens repeats progressively
    static constexpr double kStereoOffsetRatio = 1.0625; // classic "dotted" offset trick
};

} // namespace drop
