#pragma once
#include <juce_dsp/juce_dsp.h>
#include <random>

/**
 * NoiseModule - supplementary white/pink/filtered noise riser energy.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > NoiseModule
 * and plugins/DROP/.ideas/implementation-spec.md > NOISEMODULE.
 *
 * std::mt19937-seeded (NEVER juce::Random, per the plugin-wide determinism
 * requirement) - two independent streams (L/R), deterministically derived
 * from a single master seed: seedL = seed, seedR = seed ^ 0x9E3779B9
 * (architecture.md's exact, documented derivation).
 *
 * Parallel ADD, not a series insert (architecture.md Processing Chain
 * "Routing notes": "NoiseModule is a parallel add... summed into the bus
 * after DigitalModule"). The RNG streams advance every sample regardless of
 * `weight` (architecture.md: "a deliberate exception to 'always process =
 * same CPU cost'... needed specifically to keep Chaos-seeded
 * reproducibility independent of automation history") - only the
 * CONTRIBUTION to the bus is weight-scaled.
 *
 * RT-safe: no allocation in process() (std::uniform_real_distribution has
 * no heap state); Paul Kellett's cascaded one-pole pink-noise approximation
 * for the Pink color.
 */
namespace drop
{

enum class NoiseColor
{
    White = 0,
    Pink,
    Filtered
};

class NoiseModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    /** Reseeds both L/R streams from `masterSeed` (architecture.md's documented derivation). Call at Arming. */
    void setSeed(uint32_t masterSeed) noexcept;

    void setColor(NoiseColor newColor) noexcept { color = newColor; }
    NoiseColor getColor() const noexcept { return color; }

    /**
     * Adds noise into `block` (parallel add, never replaces existing
     * content). `weight` = IntensityBudget weight (0-1). `progress` drives
     * the rise envelope + brightness sweep. `stereoWidth01` blends
     * correlated (0) vs independent (1) L/R streams.
     */
    void process(juce::dsp::AudioBlock<float> block, float weight, float progress, float stereoWidth01) noexcept;

    static constexpr uint32_t kSeedXorConstant = 0x9E3779B9u;

private:
    struct PinkFilterState
    {
        float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, b3 = 0.0f, b4 = 0.0f, b5 = 0.0f, b6 = 0.0f;
    };

    float nextWhite(std::mt19937& rng) noexcept;
    float nextPink(std::mt19937& rng, PinkFilterState& state) noexcept;

    std::mt19937 rngL, rngR;
    PinkFilterState pinkStateL, pinkStateR;
    float brightnessStateL = 0.0f, brightnessStateR = 0.0f; // one-pole LP state (brightness sweep)

    juce::SmoothedValue<float> weightSmoothed;
    double sampleRate = 44100.0;
    NoiseColor color = NoiseColor::White;
};

} // namespace drop
