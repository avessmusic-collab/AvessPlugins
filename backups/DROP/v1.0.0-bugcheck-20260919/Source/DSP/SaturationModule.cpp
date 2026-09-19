#include "SaturationModule.h"
#include <cmath>

namespace drop
{

namespace
{
    constexpr double kWeightSmoothingSeconds = 0.02;
}

void SaturationModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    const size_t numChannels = (size_t) juce::jmax(1u, spec.numChannels);
    const int maxBlock = (int) juce::jmax(1u, spec.maximumBlockSize);

    // Both oversampling factors always pre-built (architecture.md - never
    // allocated in process()); which one runs (if any) is chosen per
    // segment based on effective drive.
    oversampler2x = std::make_unique<juce::dsp::Oversampling<float>>(
        numChannels, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    oversampler4x = std::make_unique<juce::dsp::Oversampling<float>>(
        numChannels, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);

    oversampler2x->initProcessing((size_t) maxBlock);
    oversampler4x->initProcessing((size_t) maxBlock);

    dryBuffer.setSize((int) numChannels, maxBlock);
    wetBuffer.setSize((int) numChannels, maxBlock);

    juce::dsp::ProcessSpec monoSpec { spec.sampleRate, spec.maximumBlockSize, 1 };
    auto dcCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(spec.sampleRate, 5.0);
    for (auto& f : dcBlockers)
    {
        f.coefficients = dcCoeffs;
        f.prepare(monoSpec);
    }

    weightSmoothed.reset(spec.sampleRate, kWeightSmoothingSeconds);

    reset();
}

void SaturationModule::reset()
{
    if (oversampler2x != nullptr) oversampler2x->reset();
    if (oversampler4x != nullptr) oversampler4x->reset();
    for (auto& f : dcBlockers)
        f.reset();
    weightSmoothed.setCurrentAndTargetValue(0.0f);
}

float SaturationModule::shape(float x, SaturationMode m) noexcept
{
    switch (m)
    {
        case SaturationMode::Clean:
            // Near-linear, subtle soft-knee only above a high threshold.
            return std::tanh(x * 0.6f);

        case SaturationMode::Warm:
            // tanh with mild 2nd-harmonic-biased asymmetry.
            return std::tanh(x + 0.2f * x * x);

        case SaturationMode::Driven:
            return std::atan(x) * (2.0f / juce::MathConstants<float>::pi);

        case SaturationMode::Distorted:
            // Soft division-based clip, bounded to [-1, 1] by construction.
            return juce::jlimit(-1.0f, 1.0f, x / (1.0f + std::abs(x)));

        case SaturationMode::Crushed:
            // Aggressive asymmetric clip (intentional controlled character
            // at low drive per architecture.md, fully oversampled when
            // driven hard).
            return x >= 0.0f ? std::tanh(x * 1.8f) : juce::jlimit(-1.0f, 1.0f, x * 2.4f);
    }
    return x;
}

void SaturationModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0 || oversampler2x == nullptr || oversampler4x == nullptr)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);
    progress = juce::jlimit(0.0f, 1.0f, progress);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        dryBuffer.copyFrom(ch, 0, block.getChannelPointer((size_t) ch), numSamples);
        wetBuffer.copyFrom(ch, 0, dryBuffer, ch, 0, numSamples);
    }

    // Placeholder fixed drive curve (Phase 3.2 - Style-driven in Phase 3.5):
    // drive rises with progress*weight together.
    const float driveLinear = 1.0f + (juce::Decibels::decibelsToGain(kMaxDriveDb) - 1.0f) * progress * weight;

    auto wetBlockFull = juce::dsp::AudioBlock<float>(wetBuffer);
    auto wetSub = wetBlockFull.getSubBlock(0, (size_t) numSamples);

    const bool useOversampling = driveLinear > kOversampleThresholdDriveLinear
                               && (mode == SaturationMode::Driven || mode == SaturationMode::Distorted || mode == SaturationMode::Crushed);

    if (useOversampling)
    {
        auto& os = (mode == SaturationMode::Crushed) ? *oversampler4x : *oversampler2x;
        auto upBlock = os.processSamplesUp(wetSub);
        for (size_t ch = 0; ch < upBlock.getNumChannels(); ++ch)
        {
            auto* data = upBlock.getChannelPointer(ch);
            for (size_t i = 0; i < upBlock.getNumSamples(); ++i)
                data[i] = shape(data[i] * driveLinear, mode);
        }
        os.processSamplesDown(wetSub);
    }
    else
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = wetBuffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
                data[i] = shape(data[i] * driveLinear, mode);
        }
    }

    // DC blocking (AUDIO QUALITY: "Use DC blocking where appropriate" -
    // asymmetric waveshaping, especially Crushed, can introduce DC offset).
    for (int ch = 0; ch < juce::jmin(numChannels, 2); ++ch)
    {
        auto* data = wetBuffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
            data[i] = dcBlockers[(size_t) ch].processSample(data[i]);
    }

    // Final weight-mix blend - exact transparency at weight == 0.
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
