#include "ReverbModule.h"

namespace drop
{

namespace
{
    constexpr double kWeightSmoothingSeconds = 0.02;
}

void ReverbModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    dryBuffer.setSize((int) juce::jmax(1u, spec.numChannels), (int) juce::jmax(1u, spec.maximumBlockSize));
    wetBuffer.setSize((int) juce::jmax(1u, spec.numChannels), (int) juce::jmax(1u, spec.maximumBlockSize));

    reverb.~Reverb();               // juce::dsp::Reverb is non-copyable: destroy-and-
    new (&reverb) juce::dsp::Reverb(); // placement-reconstruct for a truly fresh object // DEPTH-AUDIT FIX (2026-09-20): see ImpactEngine::prepare() - smoother state survives reset(); reconstruct for bit-deterministic re-prepares
    reverb.prepare(spec);

    sendWeightSmoothed.reset(spec.sampleRate, kWeightSmoothingSeconds);
    returnWeightSmoothed.reset(spec.sampleRate, kWeightSmoothingSeconds);

    reset();
}

void ReverbModule::reset()
{
    reverb.reset();
    sendWeightSmoothed.setCurrentAndTargetValue(0.0f);
    returnWeightSmoothed.setCurrentAndTargetValue(0.0f);
}

void ReverbModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept
{
    processWithSendReturn(block, weight, weight, progress);
}

void ReverbModule::processWithSendReturn(juce::dsp::AudioBlock<float> block, float sendWeight, float returnWeight, float progress) noexcept
{
    progress = juce::jlimit(0.0f, 1.0f, progress);

    // Module always internally 100% wet (architecture.md - "the module's
    // dry/wet is fully controlled externally"). roomSize gently rises with
    // progress as a light-ambience placeholder (Style-authored in Phase 3.5).
    juce::dsp::Reverb::Parameters params;
    params.roomSize = juce::jmap(progress, 0.25f, 0.55f);
    params.damping = 0.5f;
    params.width = 1.0f;
    params.freezeMode = 0.0f; // never engaged (architecture.md: avoids CPU spikes/unbounded tails)

    processInternal(block, sendWeight, returnWeight, params);
}

void ReverbModule::processWithParams(juce::dsp::AudioBlock<float> block, float weight, const juce::dsp::Reverb::Parameters& params) noexcept
{
    processInternal(block, weight, weight, params);
}

void ReverbModule::processWithParamsSendReturn(juce::dsp::AudioBlock<float> block, float sendWeight, float returnWeight, const juce::dsp::Reverb::Parameters& params) noexcept
{
    processInternal(block, sendWeight, returnWeight, params);
}

void ReverbModule::processInternal(juce::dsp::AudioBlock<float> block, float sendWeight, float returnWeight, const juce::dsp::Reverb::Parameters& paramsIn) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    sendWeight = juce::jlimit(0.0f, 1.0f, sendWeight);
    returnWeight = juce::jlimit(0.0f, 1.0f, returnWeight);

    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom(ch, 0, block.getChannelPointer((size_t) ch), numSamples);

    // POST-1.0 bugfix: the signal FED INTO the reverb algorithm is gated by
    // sendWeight (0 during Peak/Impact/Recovery for the shared-budget
    // instance - see PluginProcessor::processSegment() - so no fresh
    // ducked-bus material re-excites the tail); the reverb algorithm itself
    // is still called every segment regardless, so its own already-excited
    // internal state keeps decaying naturally, exactly like a real
    // physical space continuing to ring after its input stops.
    sendWeightSmoothed.setTargetValue(sendWeight);
    for (int i = 0; i < numSamples; ++i)
    {
        const float sw = sendWeightSmoothed.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
            wetBuffer.setSample(ch, i, dryBuffer.getSample(ch, i) * sw);
    }

    juce::dsp::Reverb::Parameters params = paramsIn;
    params.wetLevel = 1.0f;
    params.dryLevel = 0.0f;
    reverb.setParameters(params);

    auto wetBlockFull = juce::dsp::AudioBlock<float>(wetBuffer);
    auto wetSub = wetBlockFull.getSubBlock(0, (size_t) numSamples);
    juce::dsp::ProcessContextReplacing<float> wetContext(wetSub);
    reverb.process(wetContext);

    returnWeightSmoothed.setTargetValue(returnWeight);
    for (int i = 0; i < numSamples; ++i)
    {
        const float rw = returnWeightSmoothed.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = dryBuffer.getSample(ch, i);
            const float wet = wetBuffer.getSample(ch, i);
            block.getChannelPointer((size_t) ch)[i] = dry + (wet - dry) * rw;
        }
    }
}

} // namespace drop
