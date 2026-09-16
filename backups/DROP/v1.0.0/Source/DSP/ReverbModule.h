#pragma once
#include <juce_dsp/juce_dsp.h>

/**
 * ReverbModule - low-CPU stereo reverb (juce::dsp::Reverb, modern API only).
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > ReverbModule
 * and plugins/DROP/.ideas/implementation-spec.md > REVERBMODULE.
 *
 * Uses juce::dsp::Reverb's modern prepare(ProcessSpec)/process(ProcessContext)
 * API exclusively (troubleshooting/patterns/juce8-critical-patterns.md #17 -
 * never juce::Reverb's legacy processMono()/processStereo()). The module is
 * ALWAYS internally 100% wet (wetLevel=1, dryLevel=0) - its own dry/wet mix
 * is fully controlled externally via the weight-mix blend at the end of
 * process(), exactly like every other IntensityBudget-managed module
 * (architecture.md ReverbModule config).
 *
 * Stage 3 DSP Phase 3.2 scope: roomSize/damping are fixed placeholder
 * curves (a light-ambience roomSize rise with progress); DropStyle-authored
 * size/decay/damping land in Phase 3.5.
 *
 * Stage 3 DSP Phase 3.4 addition: processWithParams() lets a caller (e.g. a
 * SEPARATE ReverbModule instance owned by ImpactEngine) supply explicit
 * juce::dsp::Reverb::Parameters directly, instead of process()'s fixed
 * progress-driven Building-phase ambience curve - architecture.md ReverbModule
 * config: "A second, shorter-decay juce::dsp::Reverb instance... is used for
 * the Impact-state short tail so the long Building-phase ambience settings
 * don't leak into the punchy post-Impact tail." Both entry points share the
 * same underlying wet/dry weight-mix blend (weightSmoothed) and always-100%-
 * internally-wet convention. freezeMode is never engaged in either (architecture.md:
 * "avoids CPU spikes and unbounded tails").
 */
namespace drop
{

class ReverbModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    void process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept;

    /**
     * Same wet/dry weight-mix blend as process(), but with the reverb's
     * room/damping/width/freeze `params` supplied explicitly by the caller
     * rather than derived from `progress` - used by ImpactEngine's dedicated
     * short-tail instance (Stage 3 DSP Phase 3.4). `params.wetLevel`/
     * `dryLevel` are overwritten internally (always 1.0/0.0, matching the
     * class-level "always process fully wet internally" convention) - the
     * caller only needs to set roomSize/damping/width/freezeMode.
     */
    void processWithParams(juce::dsp::AudioBlock<float> block, float weight, const juce::dsp::Reverb::Parameters& params) noexcept;

private:
    void processInternal(juce::dsp::AudioBlock<float> block, float weight, const juce::dsp::Reverb::Parameters& params) noexcept;

    juce::dsp::Reverb reverb;
    juce::AudioBuffer<float> dryBuffer, wetBuffer;
    juce::SmoothedValue<float> weightSmoothed;

    double sampleRate = 44100.0;
};

} // namespace drop
