#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

/**
 * MusicalClock - single source of truth for tempo/time-signature/transport.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > MusicalClock
 * and plugins/DROP/.ideas/implementation-spec.md > MUSICALCLOCK.
 *
 * Wraps juce::AudioPlayHead::PositionInfo. Converts musical divisions
 * (1/1-1/32, straight/triplet) and bar counts into exact sample counts for
 * the current sample rate/tempo. NEVER uses wall-clock time (juce::Time,
 * std::chrono) anywhere - the standalone fallback transport (used when no
 * host playhead is available, e.g. a bare AudioProcessor under test, or the
 * Standalone app with no internal transport) is driven purely by a running
 * sample counter supplied by the caller, not by real time.
 *
 * RT-safe: no allocation, no locks. update() should be called once per host
 * processBlock() (architecture.md: "recomputes samplesPerBeat once per
 * block... never cache across blocks"), before any conversion methods are
 * used for that block's segmentation.
 */
namespace drop
{

enum class MusicalDivision
{
    Whole = 0,      // 1/1
    Half,           // 1/2
    Quarter,        // 1/4
    Eighth,         // 1/8
    Sixteenth,      // 1/16
    ThirtySecond    // 1/32
};

class MusicalClock
{
public:
    MusicalClock() = default;

    /** (Re)initializes for a new sample rate. Called from prepareToPlay(). */
    void prepare(double newSampleRate) noexcept;

    /**
     * Refreshes bpm/time-signature/bar-position from the host playhead (if
     * valid), or advances the standalone fallback transport otherwise.
     *
     * @param playHead                   the processor's AudioPlayHead (may be nullptr)
     * @param numSamplesSinceLastUpdate  samples elapsed since the previous
     *                                   update() call (typically the just-
     *                                   completed host block's length) -
     *                                   used ONLY to advance the standalone
     *                                   fallback's internal sample counter,
     *                                   never a wall-clock read.
     */
    void update(juce::AudioPlayHead* playHead, int numSamplesSinceLastUpdate) noexcept;

    double getBpm() const noexcept { return bpm; }
    int getTimeSigNumerator() const noexcept { return timeSigNumerator; }
    int getTimeSigDenominator() const noexcept { return timeSigDenominator; }

    /** True when the last update() call had no valid host tempo/transport and the fixed-BPM fallback is active. */
    bool isUsingFallbackTransport() const noexcept { return !hasValidPlayHead; }

    double getSamplesPerBeat() const noexcept;
    double getBarLengthSamples() const noexcept;

    /** Exact sample count for one instance of `division`, straight or triplet (triplet = 2/3 of the straight duration). */
    double divisionToSamples(MusicalDivision division, bool triplet) const noexcept;

    /** Exact sample count for `numBars` bars at the current tempo/time-signature. */
    double barsToSamples(int numBars) const noexcept;

    /**
     * Samples remaining until the next bar boundary, based on the position
     * observed at the most recent update() call (0 if already exactly on a
     * boundary). Used by DropStateMachine's Arming state to quantize a
     * build's start to a musically sensible point.
     */
    double samplesToNextBarBoundary() const noexcept;

private:
    static double beatsForDivision(MusicalDivision division) noexcept;

    double sampleRate = 44100.0;
    double bpm = kFallbackBpm;
    int timeSigNumerator = kFallbackTimeSigNumerator;
    int timeSigDenominator = kFallbackTimeSigDenominator;
    bool hasValidPlayHead = false;

    // Position within the current bar, in samples (0 <= value < barLengthSamples),
    // latched at the most recent update() call.
    double samplePositionInBar = 0.0;

    // Only advanced when no valid host playhead is present - a running
    // sample counter, NEVER wall-clock time. Not reset by prepare() other
    // than to zero (a fresh transport position for a freshly (re)prepared
    // processor is the correct, deterministic starting point).
    double standaloneSamplePosition = 0.0;

    static constexpr double kFallbackBpm = 120.0;
    static constexpr int kFallbackTimeSigNumerator = 4;
    static constexpr int kFallbackTimeSigDenominator = 4;
};

} // namespace drop
