#include "StutterModule.h"
#include <cmath>

namespace drop
{

void StutterModule::prepare(const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    // Longest supported stutter length (1/4 note) at the lowest supported
    // tempo (40 BPM, mirrors DelayModule's worst-case convention), times a
    // safety factor covering Half mode's up-to-2x real-time loop duration
    // plus the dual-voice crossfade overlap margin.
    const double quarterNoteAt40Bpm = (60.0 / 40.0) * sampleRate;
    captureBufferLength = (int) std::ceil(quarterNoteAt40Bpm * 3.0) + 256;

    captureBuffer.setSize((int) juce::jmax(1u, spec.numChannels), captureBufferLength);
    captureBuffer.clear();

    pitchVariationEngine.prepare(spec, 30.0f);

    weightSmoothed.reset(sampleRate, 0.02);

    reset();
}

void StutterModule::reset()
{
    writePosition = 0;
    for (auto& v : voices)
    {
        v.active = false;
        v.contentPos = 0.0;
        v.contentLength = 1.0;
        v.sourceStart = 0;
    }
    activeVoice = 0;
    crossfadePos = -1.0;
    pitchVariationEngine.reset();
    weightSmoothed.setCurrentAndTargetValue(0.0f);
    captureBuffer.clear();
}

float StutterModule::readCaptureInterpolated(int channel, double absolutePosition) const noexcept
{
    if (captureBufferLength <= 0)
        return 0.0f;

    double wrapped = std::fmod(absolutePosition, (double) captureBufferLength);
    if (wrapped < 0.0)
        wrapped += (double) captureBufferLength;

    const int i0 = (int) wrapped;
    const int i1 = (i0 + 1) % captureBufferLength;
    const float frac = (float) (wrapped - (double) i0);

    const float s0 = captureBuffer.getSample(channel, i0);
    const float s1 = captureBuffer.getSample(channel, i1);
    return s0 + (s1 - s0) * frac;
}

void StutterModule::triggerVoice(Voice& voice, double contentLength) noexcept
{
    voice.active = true;
    voice.contentPos = 0.0;
    voice.contentLength = juce::jmax(1.0, contentLength);
    // Freeze the most-recently-captured window as this cycle's loop content
    // (architecture.md: "playback logic reads back from this buffer" - a
    // fresh snapshot each loop cycle gives a naturally evolving texture as
    // the underlying source material changes across the build).
    voice.sourceStart = writePosition - (int64_t) std::ceil(voice.contentLength);
}

double StutterModule::computeStutterLengthSamples(float progress, const MusicalClock& clock) const noexcept
{
    // implementation-spec.md STUTTERMODULE: "Lengths from 1/4 through
    // 1/64" - log-period interpolation across the full Quarter->SixtyFourth
    // range, then snap to the nearest legal division, mirroring GateModule's
    // established quantization approach in this codebase.
    static constexpr MusicalDivision kCandidates[] = {
        MusicalDivision::Quarter, MusicalDivision::Eighth, MusicalDivision::Sixteenth,
        MusicalDivision::ThirtySecond, MusicalDivision::SixtyFourth
    };

    const double startPeriod = clock.divisionToSamples(MusicalDivision::Quarter, false);
    const double endPeriod = clock.divisionToSamples(MusicalDivision::SixtyFourth, false);
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
    return juce::jmax(4.0, best); // hard floor: guarantees a usable content window even at extreme tempos
}

void StutterModule::process(juce::dsp::AudioBlock<float> block, float weight, float progress, const MusicalClock& clock) noexcept
{
    const int numSamples = (int) block.getNumSamples();
    const int numChannels = (int) block.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0 || captureBufferLength <= 0)
        return;

    weight = juce::jlimit(0.0f, 1.0f, weight);

    const double stutterLength = computeStutterLengthSamples(progress, clock);
    const double fadeSamples = juce::jmax(1.0, juce::jmin(stutterLength * 0.25, (double) kFadeMs * 0.001 * sampleRate));
    const bool usePitchVariation = (mode == StutterMode::PitchVariation);
    const double rateMag = (mode == StutterMode::Half) ? 0.5 : (mode == StutterMode::Double) ? 2.0 : 1.0;
    const bool reverse = (mode == StutterMode::Reverse);

    if (!voices[(size_t) activeVoice].active)
        triggerVoice(voices[(size_t) activeVoice], stutterLength);

    if (usePitchVariation)
        pitchVariationEngine.setRatio(PitchModule::ratioForSemitones(kPitchVariationSemitones));

    weightSmoothed.setTargetValue(weight);

    for (int i = 0; i < numSamples; ++i)
    {
        // Continuous capture (architecture.md: "continuously written every
        // block") - happens BEFORE this sample's output/trigger logic so a
        // freshly-triggered voice's snapshot validly includes it.
        const int captureIndex = (int) (writePosition % captureBufferLength);
        for (int ch = 0; ch < numChannels; ++ch)
            captureBuffer.setSample(ch, captureIndex, block.getChannelPointer((size_t) ch)[i]);
        ++writePosition;

        auto& active = voices[(size_t) activeVoice];
        auto& other = voices[(size_t) (1 - activeVoice)];

        active.contentPos += rateMag;

        if (crossfadePos < 0.0 && active.contentPos >= active.contentLength - fadeSamples)
        {
            triggerVoice(other, stutterLength);
            crossfadePos = 0.0;
        }

        const bool crossfading = crossfadePos >= 0.0;
        if (crossfading)
            other.contentPos += rateMag;

        const double activeContentIndex = reverse ? (active.contentLength - 1.0 - active.contentPos) : active.contentPos;
        const double activeAbsolutePos = (double) active.sourceStart + activeContentIndex;

        double otherAbsolutePos = 0.0;
        float gainOut = 1.0f, gainIn = 0.0f;
        if (crossfading)
        {
            const double otherContentIndex = reverse ? (other.contentLength - 1.0 - other.contentPos) : other.contentPos;
            otherAbsolutePos = (double) other.sourceStart + otherContentIndex;

            const float x = juce::jlimit(0.0f, 1.0f, (float) (crossfadePos / fadeSamples));
            gainOut = std::cos(x * juce::MathConstants<float>::halfPi);
            gainIn = std::sin(x * juce::MathConstants<float>::halfPi);
        }

        if (usePitchVariation)
            pitchVariationEngine.beginSample();

        const float wetMixNow = weightSmoothed.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float sample = readCaptureInterpolated(ch, activeAbsolutePos);
            if (crossfading)
            {
                const float otherSample = readCaptureInterpolated(ch, otherAbsolutePos);
                sample = sample * gainOut + otherSample * gainIn;
            }

            if (usePitchVariation)
                sample = pitchVariationEngine.processChannel(ch, sample);

            auto* data = block.getChannelPointer((size_t) ch);
            const float dry = data[i];
            data[i] = dry + (sample - dry) * wetMixNow;
        }

        if (crossfading)
        {
            crossfadePos += 1.0;
            if (crossfadePos >= fadeSamples)
            {
                crossfadePos = -1.0;
                activeVoice = 1 - activeVoice;
            }
        }
    }
}

} // namespace drop
