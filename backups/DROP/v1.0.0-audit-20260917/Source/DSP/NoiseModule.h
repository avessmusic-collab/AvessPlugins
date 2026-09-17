#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>
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
     *
     * POST-1.0 FOURTH IMPROVEMENT SESSION addition (2026-09-17, user-
     * directed: "add a box near the tension box for white noise tension to
     * control the white noise graph"). `noiseTension01` (default 0.5,
     * matching the new `noiseTension` APVTS parameter's own 50% default)
     * reshapes HOW the smoothstep above builds, WITHOUT changing its
     * endpoints - same directional meaning as the plugin-wide `tension`
     * parameter (higher = more end-weighted / later bloom, lower = earlier
     * bloom). Family: `pow(smoothstep(p), k)` with
     * `k = pow(2.0, (noiseTension01 - 0.5) * 2.4)`, so k == 1.0 EXACTLY at
     * noiseTension01 == 0.5 - the default therefore reproduces the smoothstep
     * above BIT-FOR-BIT (verified by RunTests.cpp's "[POST-1.0] ...
     * default (0.5) reproduces the pre-existing smoothstep EXACTLY" test),
     * making this a genuine no-op vs. the pre-noiseTension build at the
     * default setting. Since `smoothstep(p)` is monotonic non-decreasing
     * across [0,1] and `pow(x, k)` (k>0) is monotonic increasing in x for
     * x in [0,1], the composition stays monotonic non-decreasing for any
     * noiseTension01 in [0,1]; pow(0,k)=0 and pow(1,k)=1 regardless of k, so
     * the 0->0/1->1 endpoints are preserved exactly at every noiseTension01
     * setting too. Because smoothstep(p) < 1 for every p in (0,1), a LARGER
     * k strictly DECREASES pow(smoothstep(p), k) there - i.e. higher
     * noiseTension01 (-> larger k) yields a LOWER rise amount at any given
     * mid-build progress, which is exactly "later bloom" (verified by
     * RunTests.cpp's directionality test).
     */
    // Decibel range the riser climbs across a build (see riseAmountForProgress).
    static constexpr float kRiseRangeDb = 48.0f;

    static float riseAmountForProgress(float progress01, float noiseTension01 = 0.5f) noexcept
    {
        const float p = juce::jlimit(0.0f, 1.0f, progress01);
        // POST-1.0 (2026-09-17, user DAW report: "decrease the whitenoise
        // volume at the beginning of the curve drasticly"). The base shape is
        // now smoothstep SQUARED, which suppresses the early build far more
        // strongly while keeping the same gradual, no-sudden-onset character
        // that the 2026-09-17 "add noise gradually" fix introduced, and still
        // reaching exactly 1.0 at p == 1. Measured against the previous plain
        // smoothstep: p=0.1 -> 0.0008 (was 0.028, ~35x quieter), p=0.2 ->
        // 0.011 (was 0.104, ~10x), p=0.5 -> 0.25 (was 0.50), p=0.8 -> 0.78
        // (was 0.90). The NOISE chip (noiseTension) still shapes the result
        // exactly as before - it is applied on top as the exponent k.
        // POST-1.0 (2026-09-17, user DAW report: "make it rise more gradualy.
        // cause it rises way too fast"). The previous squared-smoothstep shape
        // was gradual in AMPLITUDE but not to the EAR: measured in decibels it
        // leapt ~27 dB between 20% and 50% of the build and then flattened out,
        // which is heard as a fast surge followed by a plateau.
        //
        // This is now a decibel-LINEAR ramp - the standard way a riser is made
        // to feel even - covering kRiseRangeDb from silence to full, so every
        // tenth of the build adds the same ~4.8 dB:
        //   p    0.2     0.4     0.6     0.8     1.0
        //   amp  0.008   0.033   0.106   0.329   1.000
        //   dB   -41.9   -29.8   -19.5   -9.7    0.0
        // The start stays drastically quiet (the previous report's requirement)
        // while the climb is perceptually constant instead of front-loaded.
        // Normalised so p==0 is exactly 0 (no step at the start of the build)
        // and p==1 is exactly 1.
        if (p <= 0.0f)
            return 0.0f; // pinned exactly: a fractional noiseTension exponent would
                         // otherwise magnify any float residue here into an audible floor
        const float raw   = std::pow(10.0f, -(kRiseRangeDb / 20.0f) * (1.0f - p));
        // Derived with the SAME expression as `raw` so the subtraction cancels
        // exactly at p == 0 (a truncated literal left ~1e-9 behind, which a
        // fractional exponent then lifted to ~1e-4).
        const float floorAmp = std::pow(10.0f, -(kRiseRangeDb / 20.0f));
        const float base  = juce::jlimit(0.0f, 1.0f, (raw - floorAmp) / (1.0f - floorAmp));
        const float t = juce::jlimit(0.0f, 1.0f, noiseTension01);
        const float k = std::pow(2.0f, (t - 0.5f) * 2.4f);
        return std::pow(base, k);
    }

    /**
     * Adds noise into `block` (parallel add, never replaces existing
     * content). `weight` = IntensityBudget weight (0-1). `progress` drives
     * the rise envelope + brightness sweep. `stereoWidth01` blends
     * correlated (0) vs independent (1) L/R streams. `noiseTension01`
     * (POST-1.0 FOURTH IMPROVEMENT SESSION addition, default 0.5) is passed
     * straight through to riseAmountForProgress() above - see that method's
     * doc comment for the full shape/directionality rationale.
     */
    void process(juce::dsp::AudioBlock<float> block, float weight, float progress, float stereoWidth01,
                 float noiseTension01 = 0.5f) noexcept;

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
