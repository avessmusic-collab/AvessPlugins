#include "NoiseModule.h"
#include <cmath>

namespace drop
{

void NoiseModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;
    weightSmoothed.reset(sampleRate, 0.02);
    reset();
}

void NoiseModule::reset()
{
    pinkStateL = PinkFilterState {};
    pinkStateR = PinkFilterState {};
    brightnessStateL = brightnessStateR = 0.0f;
    weightSmoothed.setCurrentAndTargetValue(0.0f);
}

void NoiseModule::setSeed(uint32_t masterSeed) noexcept
{
    rngL.seed(masterSeed);
    rngR.seed(masterSeed ^ kSeedXorConstant);
}

float NoiseModule::nextWhite(std::mt19937& rng) noexcept
{
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    return dist(rng);
}

float NoiseModule::nextPink(std::mt19937& rng, PinkFilterState& s) noexcept
{
    const float white = nextWhite(rng);
    // Paul Kellett's refined cascaded one-pole pink-noise approximation.
    s.b0 = 0.99886f * s.b0 + white * 0.0555179f;
    s.b1 = 0.99332f * s.b1 + white * 0.0750759f;
    s.b2 = 0.96900f * s.b2 + white * 0.1538520f;
    s.b3 = 0.86650f * s.b3 + white * 0.3104856f;
    s.b4 = 0.55000f * s.b4 + white * 0.5329522f;
    s.b5 = -0.7616f * s.b5 - white * 0.0168980f;
    const float pink = s.b0 + s.b1 + s.b2 + s.b3 + s.b4 + s.b5 + s.b6 + white * 0.5362f;
    s.b6 = white * 0.115926f;
    return pink * 0.11f;
}

void NoiseModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress, float stereoWidth01) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);
    progress = juce::jlimit(0.0f, 1.0f, progress);
    stereoWidth01 = juce::jlimit(0.0f, 1.0f, stereoWidth01);

    // POST-1.0 fix (2026-09-17, user-directed: "make so the noise adds
    // gradualy"). DOCUMENTED SUPERSESSION of the Phase 3.3 placeholder
    // `progress * progress` quadratic shape: that curve stayed almost
    // inaudible until very late in the build, then arrived abruptly (the
    // exact user complaint). Replaced with the shared, pure
    // riseAmountForProgress() (smoothstep, see this class's header doc
    // comment for the full shape justification/comparison table) - the
    // SAME math Source/ui/public/index.html's TENSION CURVE display plots
    // as its second ("NOISE") curve, so what the user sees on the graph is
    // provably what they hear. This module still runs unconditionally
    // (always-process pattern); the "sharp cutoff right at Vacuum" that
    // architecture.md describes still comes from VacuumModule attenuating
    // the whole bus during Peak - see processSegment().
    //
    // The brightness sweep below is left as a plain linear jmap
    // (unchanged) - it does not share the old rise curve's "abrupt"
    // character (it was never quadratic to begin with), so no equivalent
    // gradual-ization was needed there.
    const float riseAmount = riseAmountForProgress(progress);
    const float brightnessCutoffHz = juce::jmap(progress, 0.0f, 1.0f, 400.0f, 12000.0f);
    const float lpCoeff = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * brightnessCutoffHz / (float) sampleRate);

    weightSmoothed.setTargetValue(weight);

    for (int i = 0; i < numSamples; ++i)
    {
        float rawL, rawR;
        if (color == NoiseColor::Pink)
        {
            rawL = nextPink(rngL, pinkStateL);
            rawR = nextPink(rngR, pinkStateR);
        }
        else
        {
            // White and Filtered both start from white noise; Filtered's
            // distinguishing brightness sweep is applied uniformly below.
            rawL = nextWhite(rngL);
            rawR = nextWhite(rngR);
        }

        brightnessStateL += lpCoeff * (rawL - brightnessStateL);
        brightnessStateR += lpCoeff * (rawR - brightnessStateR);

        const float mono = 0.5f * (brightnessStateL + brightnessStateR);
        const float outL = mono + (brightnessStateL - mono) * stereoWidth01;
        const float outR = mono + (brightnessStateR - mono) * stereoWidth01;

        const float w = weightSmoothed.getNextValue() * riseAmount;

        if (numChannels >= 2)
        {
            block.getChannelPointer(0)[i] += outL * w;
            block.getChannelPointer(1)[i] += outR * w;
        }
        else
        {
            block.getChannelPointer(0)[i] += mono * w;
        }
    }
}

} // namespace drop
