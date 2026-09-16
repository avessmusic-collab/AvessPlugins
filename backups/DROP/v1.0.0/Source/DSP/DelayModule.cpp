#include "DelayModule.h"
#include "MusicalClock.h"
#include <cmath>

namespace drop
{

namespace
{
    constexpr double kWeightSmoothingSeconds = 0.02;
}

void DelayModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    // Longest supported division (1/1 note) at the lowest supported tempo
    // (40 BPM) plus stereo-offset headroom, at the sample rate actually in
    // use - architecture.md: "no runtime resize is ever needed regardless
    // of which rate the host actually uses."
    const double wholeNoteAt40Bpm = (60.0 / 40.0) * 4.0 * sampleRate;
    maxDelaySamples = std::ceil(wholeNoteAt40Bpm * kStereoOffsetRatio) + 4.0;

    delayLine.setMaximumDelayInSamples((int) maxDelaySamples);
    delayLine.prepare(spec);

    juce::dsp::ProcessSpec monoSpec { spec.sampleRate, spec.maximumBlockSize, 1 };
    auto hpfCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(spec.sampleRate, kFeedbackHpfHz);
    auto lpfCoeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass(spec.sampleRate, kFeedbackLpfHz);
    for (int ch = 0; ch < 2; ++ch)
    {
        feedbackHpf[ch].coefficients = hpfCoeffs;
        feedbackLpf[ch].coefficients = lpfCoeffs;
        feedbackHpf[ch].prepare(monoSpec);
        feedbackLpf[ch].prepare(monoSpec);
    }

    dryBuffer.setSize((int) juce::jmax(1u, spec.numChannels), (int) juce::jmax(1u, spec.maximumBlockSize));

    weightSmoothed.reset(spec.sampleRate, kWeightSmoothingSeconds);

    reset();
}

void DelayModule::reset()
{
    delayLine.reset();
    for (int ch = 0; ch < 2; ++ch)
    {
        feedbackHpf[ch].reset();
        feedbackLpf[ch].reset();
    }
    weightSmoothed.setCurrentAndTargetValue(0.0f);
}

void DelayModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress, const MusicalClock& clock) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);
    progress = juce::jlimit(0.0f, 1.0f, progress);

    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom(ch, 0, block.getChannelPointer((size_t) ch), numSamples);

    // Stage 3 DSP Phase 3.5: base division is Chaos-drawn at Arming (see
    // setChaosDivision()), defaulting to the prior fixed Eighth-note
    // placeholder until set. Stereo offset per architecture.md's
    // "dotted-offset trick".
    const double baseDelaySamples = clock.divisionToSamples(chaosDivision, false);
    const double leftDelaySamples = juce::jlimit(1.0, maxDelaySamples, baseDelaySamples);
    const double rightDelaySamples = juce::jlimit(1.0, maxDelaySamples, baseDelaySamples * kStereoOffsetRatio);

    // architecture.md: "Prevent runaway feedback" is an ABSOLUTE guarantee -
    // tanh soft-clip (bounds to [-1,1]) is applied BEFORE this coefficient,
    // which is itself hard-clamped to <= kMaxFeedback, so the recursive loop
    // gain can never reach/exceed 1.0 regardless of automation extremes.
    const float feedbackAmount = juce::jlimit(0.0f, kMaxFeedback, 0.2f + 0.5f * progress * weight);

    weightSmoothed.setTargetValue(weight);

    for (int i = 0; i < numSamples; ++i)
    {
        const float w = weightSmoothed.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const double delaySamples = (ch == 0) ? leftDelaySamples : rightDelaySamples;
            const float dry = dryBuffer.getSample(ch, i);
            const float delayed = delayLine.popSample(ch, (float) delaySamples);

            const int fbCh = juce::jmin(ch, 1);
            float fb = feedbackHpf[(size_t) fbCh].processSample(delayed);
            fb = feedbackLpf[(size_t) fbCh].processSample(fb);
            fb = std::tanh(fb * kFeedbackDrive);
            fb *= feedbackAmount;

            delayLine.pushSample(ch, dry + fb);

            const float wet = delayed;
            block.getChannelPointer((size_t) ch)[i] = dry + (wet - dry) * w;
        }
    }
}

} // namespace drop
