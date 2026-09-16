#include "FilterModule.h"
#include <cmath>

namespace drop
{

namespace
{
    constexpr double kWeightSmoothingSeconds = 0.02;
}

void FilterModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    lowFilter.prepare(spec);
    highFilter.prepare(spec);

    const int numChannels = (int) juce::jmax(1u, spec.numChannels);
    const int maxBlock = (int) juce::jmax(1u, spec.maximumBlockSize);

    dryBuffer.setSize(numChannels, maxBlock);
    wetBuffer.setSize(numChannels, maxBlock);
    dualBuffer.setSize(numChannels, maxBlock);

    weightSmoothed.reset(spec.sampleRate, kWeightSmoothingSeconds);

    reset();
}

void FilterModule::reset()
{
    lowFilter.reset();
    highFilter.reset();
    weightSmoothed.setCurrentAndTargetValue(0.0f);
}

void FilterModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);
    progress = juce::jlimit(0.0f, 1.0f, progress);

    // Capture dry + seed wet scratch (preallocated in prepare(), never
    // resized here - RT-safe).
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* src = block.getChannelPointer((size_t) ch);
        dryBuffer.copyFrom(ch, 0, src, numSamples);
        wetBuffer.copyFrom(ch, 0, src, numSamples);
    }

    // Musical cutoff sweep: log-frequency interpolation from a fixed
    // placeholder start/end (architecture.md - Style-authored in Phase 3.5).
    const float logStart = std::log(kStartCutoffHz);
    const float logEnd = std::log(kEndCutoffHz);
    float targetCutoff = std::exp(logStart + (logEnd - logStart) * progress);

    // Low-Frequency Protection: never sweep below the floor in LP/Dual mode.
    if (mode == FilterMode::LowPass || mode == FilterMode::Dual)
        targetCutoff = juce::jmax(kLowFreqFloorHz, targetCutoff);

    targetCutoff = juce::jlimit(20.0f, (float) (sampleRate * 0.45), targetCutoff);

    // Resonance rises with progress*weight but is hard-clamped to the
    // ceiling ("Protect against unstable resonance").
    const float resonance = juce::jlimit(kMinResonance, kMaxResonance,
                                          kMinResonance + (kMaxResonance - kMinResonance) * progress * weight);

    lowFilter.setCutoffFrequency(targetCutoff);
    lowFilter.setResonance(resonance);
    highFilter.setCutoffFrequency(targetCutoff);
    highFilter.setResonance(resonance);

    using FT = juce::dsp::StateVariableTPTFilterType;

    auto wetBlockFull = juce::dsp::AudioBlock<float>(wetBuffer);
    auto wetSub = wetBlockFull.getSubBlock(0, (size_t) numSamples);
    juce::dsp::ProcessContextReplacing<float> wetContext(wetSub);

    if (mode == FilterMode::Dual)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            dualBuffer.copyFrom(ch, 0, dryBuffer, ch, 0, numSamples);

        lowFilter.setType(FT::lowpass);
        highFilter.setType(FT::highpass);

        lowFilter.process(wetContext);

        auto dualBlockFull = juce::dsp::AudioBlock<float>(dualBuffer);
        auto dualSub = dualBlockFull.getSubBlock(0, (size_t) numSamples);
        juce::dsp::ProcessContextReplacing<float> dualContext(dualSub);
        highFilter.process(dualContext);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* wetPtr = wetBuffer.getWritePointer(ch);
            auto* dualPtr = dualBuffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
                wetPtr[i] = 0.5f * (wetPtr[i] + dualPtr[i]);
        }
    }
    else
    {
        lowFilter.setType(mode == FilterMode::LowPass  ? FT::lowpass
                          : mode == FilterMode::HighPass ? FT::highpass
                                                          : FT::bandpass);
        lowFilter.process(wetContext);
    }

    // Final weight-mix blend (architecture.md "Always-Process, Weight-Mix
    // Architecture") - guarantees exact transparency at weight == 0 and
    // smooth (SmoothedValue-ramped) engagement otherwise.
    weightSmoothed.setTargetValue(weight);
    for (int i = 0; i < numSamples; ++i)
    {
        const float w = weightSmoothed.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = dryBuffer.getSample(ch, i);
            const float wet = wetBuffer.getSample(ch, i);
            block.getChannelPointer((size_t) ch)[i] = dry + (wet - dry) * w;
        }
    }
}

} // namespace drop
