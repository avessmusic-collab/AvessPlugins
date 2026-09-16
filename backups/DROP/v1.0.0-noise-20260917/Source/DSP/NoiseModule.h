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
     * POST-1.0 addition (2026-09-17, user-directed: "make so the noise adds
     * gradualy"). SINGLE SOURCE OF TRUTH for the noise riser's rise-amount
     * shape - a pure, stateless, deterministic function of the incoming
     * `progress` (the SAME curved-progress value process()'s own `progress`
     * argument receives, per PluginProcessor.cpp processSegment()'s decision
     * matrix). Exposed `static` (no instance state, no RNG, no side effects)
     * specifically so Source/ui/public/index.html's (and the byte-identical
     * .ideas/mockups/v10-ui.html's) TENSION CURVE display can plot the
     * EXACT same math the DSP uses for its second ("NOISE") curve, without
     * duplicating the formula in two places that could silently drift apart
     * - the JS mirror is verified to agree with this function by
     * RunTests.cpp's "[POST-1.0] noise rise gradual" test block.
     *
     * SHAPE: smoothstep, p*p*(3-2p) - chosen over a plain power curve
     * (e.g. progress^0.8, also considered) because it is genuinely
     * C1-continuous at BOTH ends (derivative is exactly 0 at p=0 and p=1;
     * a power curve with exponent < 1 has an INFINITE derivative at p=0,
     * which is not truly C1). It is monotonic non-decreasing, cheap
     * (2 multiplies, no transcendental call), fully deterministic, starts
     * at exactly 0.0 at progress=0 and reaches exactly 1.0 at progress=1,
     * and - unlike the OLD `progress*progress` shape this supersedes - is
     * clearly audible early in the build: smoothstep(0.2)=0.104 and
     * smoothstep(0.5)=0.5, versus the old quadratic's 0.04 and 0.25 at the
     * same two points (the old curve stayed almost silent until very late,
     * then arrived abruptly - exactly the user's complaint).
     */
    static float riseAmountForProgress(float progress01) noexcept
    {
        const float p = juce::jlimit(0.0f, 1.0f, progress01);
        return p * p * (3.0f - 2.0f * p);
    }

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
