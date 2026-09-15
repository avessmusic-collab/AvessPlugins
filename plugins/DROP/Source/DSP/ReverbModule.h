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
 * size/decay/damping and the second short-tail Impact instance land in
 * Phase 3.4/3.5. freezeMode is never engaged (architecture.md: "avoids CPU
 * spikes and unbounded tails").
 */
namespace drop
{

class ReverbModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    void process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept;

private:
    juce::dsp::Reverb reverb;
    juce::AudioBuffer<float> dryBuffer, wetBuffer;
    juce::SmoothedValue<float> weightSmoothed;

    double sampleRate = 44100.0;
};

} // namespace drop
