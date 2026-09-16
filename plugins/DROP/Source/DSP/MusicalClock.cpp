#include "MusicalClock.h"
#include <cmath>

namespace drop
{

void MusicalClock::prepare(double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

    // Deliberately do NOT reset bpm/timeSig/hasValidPlayHead here -
    // architecture.md: "no persistent sample-domain state is cached across
    // prepareToPlay() calls; everything is recomputed from sampleRate + bpm
    // each time it's needed." sampleRate itself is exactly what
    // prepareToPlay() is meant to (re)supply; bpm/time-sig are refreshed by
    // the next update() call regardless.
    standaloneSamplePosition = 0.0;
    samplePositionInBar = 0.0;
}

void MusicalClock::update(juce::AudioPlayHead* playHead, int numSamplesSinceLastUpdate) noexcept
{
    juce::Optional<juce::AudioPlayHead::PositionInfo> position;
    if (playHead != nullptr)
        position = playHead->getPosition();

    const auto bpmValue = position.hasValue() ? position->getBpm() : juce::Optional<double> {};

    if (bpmValue.hasValue() && *bpmValue > 0.0)
    {
        hasValidPlayHead = true;
        bpm = *bpmValue;

        // POST-1.0 addition (TRANSPORT-STOP SAFETY): PositionInfo::isPlaying
        // is a plain bool (not Optional<>, unlike bpm/timeSignature/
        // ppqPosition above) - always readable once a valid PositionInfo was
        // obtained. Only latched inside this valid-host-bpm branch, matching
        // isHostPlaying()'s documented "only meaningful when
        // isUsingFallbackTransport() == false" contract.
        hostIsPlaying = position->getIsPlaying();

        if (const auto sig = position->getTimeSignature(); sig.hasValue())
        {
            timeSigNumerator = juce::jmax(1, sig->numerator);
            timeSigDenominator = juce::jmax(1, sig->denominator);
        }

        const double samplesPerBeat = getSamplesPerBeat();
        const double beatsPerBar = timeSigNumerator * (4.0 / (double) timeSigDenominator);

        if (const auto ppq = position->getPpqPosition(); ppq.hasValue() && beatsPerBar > 0.0)
        {
            double ppqAtBarStart;
            if (const auto lastBarStart = position->getPpqPositionOfLastBarStart(); lastBarStart.hasValue())
            {
                ppqAtBarStart = *lastBarStart;
            }
            else
            {
                const double barsElapsed = std::floor(*ppq / beatsPerBar);
                ppqAtBarStart = barsElapsed * beatsPerBar;
            }

            double ppqIntoBar = *ppq - ppqAtBarStart;
            if (!std::isfinite(ppqIntoBar))
                ppqIntoBar = 0.0;
            ppqIntoBar = std::fmod(juce::jmax(0.0, ppqIntoBar), beatsPerBar);

            samplePositionInBar = ppqIntoBar * samplesPerBeat;
        }
        else
        {
            samplePositionInBar = 0.0;
        }
    }
    else
    {
        // Standalone-app / no-host-transport fallback (architecture.md:
        // "fall back to a fixed internal 120 BPM / 4/4 transport driven by
        // an internal sample counter, so DROP is still usable and
        // deterministic outside a host").
        hasValidPlayHead = false;
        // POST-1.0 addition: explicitly re-pinned to its safe default every
        // fallback call (not just left stale from a possible earlier valid-
        // host-bpm reading) - isHostPlaying() must never report a spurious
        // "false" once the fallback path is active.
        hostIsPlaying = true;
        bpm = kFallbackBpm;
        timeSigNumerator = kFallbackTimeSigNumerator;
        timeSigDenominator = kFallbackTimeSigDenominator;

        if (numSamplesSinceLastUpdate > 0)
            standaloneSamplePosition += (double) numSamplesSinceLastUpdate;

        const double barLength = getBarLengthSamples();
        samplePositionInBar = barLength > 0.0 ? std::fmod(standaloneSamplePosition, barLength) : 0.0;
    }
}

double MusicalClock::getSamplesPerBeat() const noexcept
{
    return (60.0 / juce::jmax(1.0, bpm)) * sampleRate;
}

double MusicalClock::getBarLengthSamples() const noexcept
{
    return getSamplesPerBeat() * timeSigNumerator * (4.0 / (double) juce::jmax(1, timeSigDenominator));
}

double MusicalClock::beatsForDivision(MusicalDivision division) noexcept
{
    switch (division)
    {
        case MusicalDivision::Whole:        return 4.0;
        case MusicalDivision::Half:         return 2.0;
        case MusicalDivision::Quarter:      return 1.0;
        case MusicalDivision::Eighth:       return 0.5;
        case MusicalDivision::Sixteenth:    return 0.25;
        case MusicalDivision::ThirtySecond: return 0.125;
        case MusicalDivision::SixtyFourth:  return 0.0625;
    }
    return 1.0;
}

double MusicalClock::divisionToSamples(MusicalDivision division, bool triplet) const noexcept
{
    const double straightSamples = beatsForDivision(division) * getSamplesPerBeat();
    return triplet ? straightSamples * (2.0 / 3.0) : straightSamples;
}

double MusicalClock::barsToSamples(int numBars) const noexcept
{
    return (double) juce::jmax(0, numBars) * getBarLengthSamples();
}

double MusicalClock::samplesToNextBarBoundary() const noexcept
{
    const double barLength = getBarLengthSamples();
    if (barLength <= 0.0)
        return 0.0;

    const double remainder = std::fmod(samplePositionInBar, barLength);
    if (remainder <= 1.0e-6)
        return 0.0;

    return barLength - remainder;
}

} // namespace drop
