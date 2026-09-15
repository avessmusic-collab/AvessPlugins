#pragma once
#include <cstdint>
#include <limits>

/**
 * DropStateMachine - deterministic, sample-accurate state machine driving
 * which processing regime is active.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > DropStateMachine
 * and plugins/DROP/.ideas/implementation-spec.md > STATE MACHINE.
 *
 * Plain control-rate logic, no JUCE dependency, no allocation. This class
 * never reads MIDI/APVTS/AudioPlayHead directly - the owner (PluginProcessor's
 * sample-accurate segmentation loop, architecture.md "Sample-Accurate Events
 * / Block Segmentation") is responsible for:
 *   1. Calling requestTrigger() at the exact sample offset a trigger event
 *      (parameter edge or matching MIDI note-on) occurs.
 *   2. Calling tick() repeatedly at a segment boundary to cascade through
 *      any number of exactly-zero-length states without consuming samples.
 *   3. Calling advance(n) to consume n samples within the CURRENT state
 *      (n must be <= samplesUntilNextTransition(), which the caller
 *      guarantees by computing segment lengths from it).
 */
namespace drop
{

enum class DropState
{
    Idle = 0,
    Arming,
    Building,
    Peak,
    Impact,
    Recovery
};

class DropStateMachine
{
public:
    struct Durations
    {
        // All in exact (already-latched) sample counts, computed by the
        // caller from MusicalClock/buildLength/etc. at the moment a build is
        // requested (architecture.md: tempo/time-signature for an in-flight
        // build is latched at Arming, not continuously re-derived).
        int64_t armingToBuildingSamples = 0; // samples-to-next-bar-boundary
        int64_t buildingSamples = 0;         // buildLength bars -> samples
        int64_t peakSamples = 0;             // brief Style-configurable window
        int64_t impactSamples = 0;           // Vacuum window + ImpactEngine burst/tail window
        int64_t recoverySamples = 0;         // settle window
    };

    DropState getState() const noexcept { return state; }

    /**
     * Requests Idle -> Arming, latching `durationsForThisBuild` for the
     * remainder of this build. No-op (re-trigger safety, architecture.md)
     * unless state == Idle. Returns true if the trigger was accepted.
     */
    bool requestTrigger(const Durations& durationsForThisBuild) noexcept;

    /**
     * If the current state's latched duration has been fully consumed,
     * performs exactly one transition to the next state in the table
     * (Idle is never auto-exited; it waits indefinitely for
     * requestTrigger()). Returns true if a transition occurred. Never
     * consumes samples itself - call in a loop (`while (tick()) {}`) at
     * each segment boundary to cascade through any legitimately
     * zero-length states before advancing further.
     */
    bool tick() noexcept;

    /** Consumes `numSamples` within the CURRENT state. No-op for Idle or numSamples <= 0. */
    void advance(int64_t numSamples) noexcept;

    /**
     * Samples remaining in the current state before tick() would perform a
     * transition (std::numeric_limits<int64_t>::max() for Idle, which has
     * no scheduled exit). Used by the caller to compute segment lengths.
     */
    int64_t samplesUntilNextTransition() const noexcept;

    /** Resets to Idle with zero latched durations (called from prepareToPlay() - a saved project never resumes mid-drop, per architecture.md State Persistence). */
    void reset() noexcept;

private:
    int64_t lengthOfState(DropState s) const noexcept;
    void enterState(DropState newState) noexcept;

    DropState state = DropState::Idle;
    Durations durations {};
    int64_t samplesIntoState = 0;
};

} // namespace drop
