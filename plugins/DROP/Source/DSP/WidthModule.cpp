#include "WidthModule.h"
#include <cmath>

namespace drop
{

namespace
{
    constexpr double kWeightSmoothingSeconds = 0.02;
}

void WidthModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    // One-pole LP coefficient at kCrossoverHz: coeff = 1 - exp(-2*pi*fc/fs).
    const double twoPiFcOverFs = 2.0 * juce::MathConstants<double>::pi * (double) kCrossoverHz / sampleRate;
    crossoverCoeff = (float) (1.0 - std::exp(-twoPiFcOverFs));

    lowGainSmoothed.reset(sampleRate, kWeightSmoothingSeconds);
    highGainSmoothed.reset(sampleRate, kWeightSmoothingSeconds);

    reset();
}

void WidthModule::reset()
{
    lowSideState = 0.0f;
    lowGainSmoothed.setCurrentAndTargetValue(1.0f);
    highGainSmoothed.setCurrentAndTargetValue(1.0f);
}

void WidthModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    if (block.getNumChannels() < 2 || numSamples <= 0)
        return; // mono bus: no stereo image to widen/narrow - pass through untouched

    weight = juce::jlimit(0.0f, 1.0f, weight);
    progress = juce::jlimit(0.0f, 1.0f, progress);

    auto* left = block.getChannelPointer(0);
    auto* right = block.getChannelPointer(1);

    // Narrow during build (architecture.md: narrow target ~0.1-0.4), fixed
    // placeholder curve until DropStyle lands in Phase 3.5.
    const float narrowTarget = juce::jmap(progress, 0.0f, 1.0f, 1.0f, 0.3f);

    // Both bands' targets converge to 1.0 (unity/transparent) at weight == 0
    // (architecture.md "scaling the target directly" + Phase 3.2's uniform
    // "weight scales contribution from 0 to full" requirement) - the low
    // band's target simply moves toward a MUCH more conservative value than
    // the high band's, so low-band protection is always relatively stronger
    // than high-band widening at any given nonzero weight.
    const float highBandTargetGain = juce::jlimit(0.1f, 1.8f, 1.0f + (narrowTarget - 1.0f) * weight);
    const float lowBandTargetGain = juce::jlimit(0.05f, 1.0f, 1.0f + (kLowBandProtectedGain - 1.0f) * weight);

    lowGainSmoothed.setTargetValue(lowBandTargetGain);
    highGainSmoothed.setTargetValue(highBandTargetGain);

    for (int i = 0; i < numSamples; ++i)
    {
        const float mid = 0.5f * (left[i] + right[i]);
        const float side = 0.5f * (left[i] - right[i]);

        // Complementary one-pole crossover: lowSide + highSide == side exactly.
        lowSideState += crossoverCoeff * (side - lowSideState);
        const float lowSide = lowSideState;
        const float highSide = side - lowSide;

        const float lowGain = lowGainSmoothed.getNextValue();
        const float highGain = highGainSmoothed.getNextValue();

        const float processedSide = lowSide * lowGain + highSide * highGain;

        left[i] = mid + processedSide;
        right[i] = mid - processedSide;
    }
}

} // namespace drop
