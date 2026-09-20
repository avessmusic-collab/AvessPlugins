#include "GateModule.h"
#include <cmath>

namespace drop
{

void GateModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;
    edgeRamp.reset(sampleRate, (double) kAntiClickSeconds);
    reset();
}

void GateModule::reset()
{
    phaseSamples = 0.0;
    edgeRamp.setCurrentAndTargetValue(1.0f); // gate starts open
}

double GateModule::computeStepLengthSamples(float progress, const MusicalClock& clock) const noexcept
{
    // Stage 3 DSP Phase 3.5: the fastest candidate division (both the
    // interpolation target's end-anchor AND the quantization candidate set)
    // is now `chaosDivision` (Chaos-drawn at Arming, see setChaosDivision())
    // instead of a fixed ThirtySecond - defaults to ThirtySecond, so
    // pre-3.5 behavior is reproduced exactly until a caller opts in.
    const MusicalDivision kCandidates[] = {
        MusicalDivision::Quarter, MusicalDivision::Eighth, MusicalDivision::Sixteenth, chaosDivision
    };

    const double startPeriod = clock.divisionToSamples(MusicalDivision::Quarter, false);
    const double endPeriod = clock.divisionToSamples(chaosDivision, false);
    const double logStart = std::log(juce::jmax(1.0, startPeriod));
    const double logEnd = std::log(juce::jmax(1.0, endPeriod));
    const double targetPeriod = std::exp(logStart + (logEnd - logStart) * (double) juce::jlimit(0.0f, 1.0f, progress));

    double best = clock.divisionToSamples(kCandidates[0], false);
    double bestDiff = std::abs(best - targetPeriod);
    for (auto div : kCandidates)
    {
        const double candidate = clock.divisionToSamples(div, false);
        const double diff = std::abs(candidate - targetPeriod);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = candidate;
        }
    }
    return juce::jmax(1.0, best);
}

void GateModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress, const MusicalClock& clock) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);

    const double stepLength = computeStepLengthSamples(progress, clock);
    const double dutyOnSamples = stepLength * (double) kDutyCycle;

    for (int i = 0; i < numSamples; ++i)
    {
        const double positionInStep = std::fmod(phaseSamples, stepLength);
        const float rawTarget = (positionInStep < dutyOnSamples) ? 1.0f : 0.0f;

        if (rawTarget != edgeRamp.getTargetValue())
            edgeRamp.setTargetValue(rawTarget);

        const float envelopeValue = edgeRamp.getNextValue();

        // architecture.md GateModule config: "finalGain = 1 - weight*(1 -
        // envelopeValue)" - transparent at weight == 0, full on/off gate at
        // weight == 1.
        const float finalGain = 1.0f - weight * (1.0f - envelopeValue);

        for (int ch = 0; ch < numChannels; ++ch)
            block.getChannelPointer((size_t) ch)[i] *= finalGain;

        phaseSamples += 1.0;
        if (phaseSamples >= stepLength)
            phaseSamples = std::fmod(phaseSamples, stepLength);
    }
}

} // namespace drop
