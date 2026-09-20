#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <juce_core/juce_core.h>
#include "../DSP/MusicalClock.h"

/**
 * DropRandomizer - Chaos seed management + musically-constrained
 * randomization ("Chaos changes variation amount, not whether the result
 * remains musical").
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > Data-Driven
 * Style System > DropRandomizer (Chaos) and
 * plugins/DROP/.ideas/implementation-spec.md > CHAOS.
 *
 * Wraps a single std::mt19937 (NEVER juce::Random - determinism/bit-exact
 * cross-platform reproducibility requirement, matching NoiseModule). All
 * draws are scheduled at well-defined points ONLY: drawForNewBuild() is
 * called exactly once per Arming (never per-sample/per-block), latching
 * this build's randomized choices for its entire duration
 * (architecture.md: "jitter is applied once per Arming... so a single
 * build's weight curve stays smooth and reproducible").
 *
 * Reseeded from the persisted `chaos` seed EVERY Arming (not merely once at
 * construction) - implementation-spec.md's "Same preset + seed + source
 * audio + transport must reproduce the same output" is interpreted here as:
 * for a FIXED persisted seed, every trigger produces the identical
 * randomized-choice sequence, matching the "NEW/Randomize generates a new
 * seed" UI model (the CURRENT seed governs every build until the user
 * explicitly requests a new one).
 *
 * All randomized choices are drawn from small, explicit, musically-valid
 * tables (never raw unconstrained floats) - implementation-spec.md CHAOS:
 * "Do not use random values without musical constraints."
 *
 * Pure control-rate logic; RT-safe (no allocation - std::mt19937 has fixed
 * internal state, std::uniform_*_distribution is stateless per call).
 */
namespace drop
{

class DropRandomizer
{
public:
    /** Reseeds the underlying std::mt19937 engine. Call at Arming, before drawForNewBuild(). */
    void setSeed(uint32_t newSeed) noexcept;
    uint32_t getSeed() const noexcept { return seed; }

    /** `chaos01` = the `chaos` APVTS parameter, normalized 0-1. */
    void setChaosAmount01(float chaos01) noexcept { chaosAmount01 = juce::jlimit(0.0f, 1.0f, chaos01); }
    float getChaosAmount01() const noexcept { return chaosAmount01; }

    /**
     * Draws ALL of this build's musically-constrained random choices in one
     * deterministic pass and caches them (architecture.md: "RNG draws
     * happen only at well-defined points: once at Arming"). Must be called
     * AFTER setSeed()/setChaosAmount01() for this build.
     */
    void drawForNewBuild() noexcept;

    // --- Latched results of the most recent drawForNewBuild() call -------

    /** One of architecture.md's musical semitone set (see PitchModule::quantizeSemitones). */
    int getPitchSemitoneChoice() const noexcept { return pitchSemitoneChoice; }

    MusicalDivision getDelayDivisionChoice() const noexcept { return delayDivisionChoice; }
    MusicalDivision getGateDivisionChoice() const noexcept { return gateDivisionChoice; }

    /** Index into StutterMode's 5 values (Repeat/Reverse/Half/Double/PitchVariation). */
    int getStutterModeChoice() const noexcept { return stutterModeChoice; }

    /** Bounded +-jitter (as a fraction, already scaled by chaosAmount01) for IntensityBudget module `moduleIndex` (0..9). */
    float getIntensityJitter(int moduleIndex) const noexcept;

    static constexpr int kNumIntensityModules = 10;
    static constexpr float kMaxIntensityJitter = 0.15f; // +-15% at chaos == 100%

private:
    uint32_t seed = 1;
    std::mt19937 rng;
    float chaosAmount01 = 0.0f;

    int pitchSemitoneChoice = 0;
    MusicalDivision delayDivisionChoice = MusicalDivision::Eighth;
    MusicalDivision gateDivisionChoice = MusicalDivision::Sixteenth;
    int stutterModeChoice = 0;
    std::array<float, (size_t) kNumIntensityModules> intensityJitter {};
};

} // namespace drop
