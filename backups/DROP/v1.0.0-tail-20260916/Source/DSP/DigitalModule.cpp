#include "DigitalModule.h"
#include <cmath>

namespace drop
{

void DigitalModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    juce::dsp::ProcessSpec monoSpec { spec.sampleRate, spec.maximumBlockSize, 1 };
    auto coeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, kDcBlockerHz);
    for (auto& f : dcBlockers)
    {
        f.coefficients = coeffs;
        f.prepare(monoSpec);
    }

    weightSmoothed.reset(sampleRate, 0.02);

    reset();
}

void DigitalModule::reset()
{
    for (auto& f : dcBlockers)
        f.reset();
    heldSample[0] = heldSample[1] = 0.0f;
    srCounterPhase[0] = srCounterPhase[1] = 0.0;
    weightSmoothed.setCurrentAndTargetValue(0.0f);
}

void DigitalModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress, float tension01, float chaos01) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);
    progress = juce::jlimit(0.0f, 1.0f, progress);
    tension01 = juce::jlimit(0.0f, 1.0f, tension01);
    chaos01 = juce::jlimit(0.0f, 1.0f, chaos01);

    // architecture.md: "intentionally near-zero for most of the build and
    // only rises sharply in the final tension-heavy portion and/or scales
    // with chaos" - Phase 3.3 fixed placeholder curve (real Style-authored
    // activation curve lands in Phase 3.5).
    const float severity = juce::jlimit(0.0f, 1.0f, progress * (0.15f + 0.55f * tension01 + 0.45f * chaos01));
    const float bitDepth = juce::jmap(severity, 0.0f, 1.0f, kMaxBitDepth, kMinBitDepth);
    const float srRatio = juce::jmap(severity, 0.0f, 1.0f, 1.0f, kMinSrRatio);
    const float scale = std::pow(2.0f, bitDepth - 1.0f);

    weightSmoothed.setTargetValue(weight);

    for (int i = 0; i < numSamples; ++i)
    {
        const float wetMix = weightSmoothed.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = block.getChannelPointer((size_t) ch);
            const float dry = data[i];

            srCounterPhase[ch] += (double) srRatio;
            if (srCounterPhase[ch] >= 1.0)
            {
                srCounterPhase[ch] -= std::floor(srCounterPhase[ch]);
                heldSample[ch] = std::round(dry * scale) / scale;
            }

            float wet = heldSample[ch];
            wet = dcBlockers[(size_t) ch].processSample(wet);

            data[i] = dry + (wet - dry) * wetMix;
        }
    }
}

} // namespace drop
