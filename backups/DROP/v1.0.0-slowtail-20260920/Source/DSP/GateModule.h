#pragma once
#include <juce_dsp/juce_dsp.h>
#include "MusicalClock.h"

/**
 * GateModule - tempo-synced rhythmic envelope gate with anti-click ramps.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > GateModule
 * and plugins/DROP/.ideas/implementation-spec.md > GATEMODULE.
 *
 * Stage 3 DSP Phase 3.2 scope: straight-division rate interpolation only
 * (Quarter -> ThirtySecond, quantized to the nearest of 4 candidate
 * divisions via log-period interpolation driven by `progress`); triplet/
 * accelerating-vs-decelerating pattern variants and Chaos-driven ghost-step
 * probability are Style-authored behavior deferred to Phase 3.5.
 *
 * `finalGain = 1 - weight*(1 - envelopeValue)` (architecture.md's exact
 * formula) means the module is bit-exactly transparent at weight == 0
 * without needing a separate dry/wet scratch buffer - the "always process,
 * weight-mix" pattern is baked directly into the gain formula.
 *
 * RT-safe: no allocation, no scratch buffers - all state is a running phase
 * position (samples) plus one SmoothedValue for the anti-click envelope.
 */
namespace drop
{

class GateModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    /** `clock` supplies the current tempo for musical-division rate quantization. */
    void process(juce::dsp::AudioBlock<float> block, float weight, float progress, const MusicalClock& clock) noexcept;

    /**
     * Stage 3 DSP Phase 3.5: the fastest (progress==1) end of the rate
     * sweep is quantized toward this Chaos-drawn division instead of the
     * fixed ThirtySecond placeholder (architecture.md Integration Points:
     * "DropRandomizer (Chaos) -> ... GateModule pattern/rate" - latched once
     * per Arming via DropRandomizer::getGateDivisionChoice(), a
     * musically-constrained choice, never an arbitrary value). Defaults to
     * ThirtySecond (identical to the pre-3.5 fixed behavior) until called.
     */
    void setChaosDivision(MusicalDivision division) noexcept { chaosDivision = division; }

private:
    double computeStepLengthSamples(float progress, const MusicalClock& clock) const noexcept;

    double sampleRate = 44100.0;
    double phaseSamples = 0.0; // running position within the current gate step, persists across segments/blocks
    juce::SmoothedValue<float> edgeRamp; // anti-click envelope (linear ramp approximation of architecture.md's raised-cosine)
    MusicalDivision chaosDivision = MusicalDivision::ThirtySecond;

    static constexpr float kAntiClickSeconds = 0.002f; // 2ms, within architecture.md's 1-3ms target
    static constexpr float kDutyCycle = 0.5f;
};

} // namespace drop
