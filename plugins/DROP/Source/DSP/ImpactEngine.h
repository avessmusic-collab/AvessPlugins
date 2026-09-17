#pragma once
#include <juce_dsp/juce_dsp.h>
#include <random>
#include "ReverbModule.h"

/**
 * ImpactEngine - coordinates the signature Vacuum->Impact moment's IMPACT
 * half.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > ImpactEngine,
 * plugins/DROP/.ideas/implementation-spec.md > IMPACTENGINE / IMPACT
 * PHILOSOPHY / TRANSIENT PRESERVATION / LOW-FREQUENCY PROTECTION, and
 * plugins/DROP/.ideas/plan.md > Phase 3.4.
 *
 * "Impact should emphasize the original musical transient rather than
 * simply becoming a huge processed sound" (IMPACT PHILOSOPHY) - ImpactEngine
 * is a COORDINATOR, not a wall of synthesized effect: it layers a modest,
 * band-limited sub-hit + noise burst on TOP of a bus that
 * PluginProcessor::computeIntensityProgress() has already ducked the shared
 * 10 IntensityBudget modules out of for the ENTIRE Impact state
 * (TRANSIENT PRESERVATION: "duck the effect chain, not the source"), so the
 * dominant content reaching the listener at the drop is still recognizably
 * the source material. It also drives WidthModule's stereo-expansion target
 * and a light SaturationModule overlay for the SAME segment (see
 * PluginProcessor::processSegment() - those two modules are called
 * externally with drive values computed here, not owned by this class), and
 * owns a dedicated short-tail ReverbModule instance so the long-form
 * Building-phase ambience settings never leak into the punchy post-Impact
 * tail (architecture.md ReverbModule config).
 *
 * State-gated (NOT IntensityBudget weight-blended, matching VacuumModule) -
 * only produces non-identity output during the DropStateMachine `Impact`
 * state. All intensity scales with the `impact` APVTS parameter, which
 * architecture.md deliberately excludes from the shared 1.0 IntensityBudget
 * pool.
 *
 * Vacuum->Impact handoff (architecture.md VacuumModule config: "the ramp
 * back up and Impact's attack are designed together so there is no
 * double-envelope bump"): Peak's ENTIRE latched duration IS the Vacuum
 * window (see PluginProcessor::computeDurationsForNewBuild()), and
 * VacuumModule::computeGain() returns exactly 1.0 (fully released) at
 * elapsed >= totalWindowSamples, i.e. at Peak's very last sample. The
 * segmentation loop guarantees a fresh segment starts at Impact's very
 * first sample (elapsed == 0 here), so this engine's own attack begins
 * exactly where Vacuum's release ends, with no overlap.
 *
 * RT-safe: no allocation in process()/computeDriveValues()/renderSubHitOnly().
 * The burst-noise generator mirrors NoiseModule's std::mt19937 +
 * std::uniform_real_distribution pattern - a "seeded equivalent"
 * deterministic generator (architecture.md ImpactEngine config: "reuses
 * NoiseModule's RNG-derived generator").
 */
namespace drop
{

class ImpactEngine
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    /** Reseeds the burst-noise generator, deterministically derived from `masterSeed` (call at Arming, like NoiseModule::setSeed()). */
    void setSeed(uint32_t masterSeed) noexcept;

    /**
     * Coordination-drive values ImpactEngine wants applied to OTHER modules
     * THIS segment (architecture.md: ImpactEngine "drives existing modules
     * into a specific configuration rather than reimplementing width/
     * saturation... from scratch") - computed by the caller BEFORE the
     * fixed series chain runs so PluginProcessor::processSegment() can feed
     * them into the SAME-segment WidthModule/SaturationModule calls. Pure/
     * stateless: `elapsedSamplesAtSegmentStart`/`totalWindowSamples` define
     * a fast raised-cosine attack (first 8% of the window) then hold; both
     * outputs scale monotonically with `impactAmount01`.
     */
    struct DriveValues
    {
        float widthExpansionAmount = 0.0f;    // 0-1: WidthModule::process()'s expansionOverride01 argument
        float saturationOverlayAmount = 0.0f; // 0-1: blended (max) into SaturationModule's effective weight
    };
    static DriveValues computeDriveValues(double elapsedSamplesAtSegmentStart, double totalWindowSamples, float impactAmount01) noexcept;

    /**
     * Adds the sub-hit + noise burst additively into `block` (on top of
     * whatever the ducked chain already produced), applies a low-end
     * soft-clip safety net to the SUMMED result (LOW-FREQUENCY PROTECTION:
     * "Impact low-end must be controlled"), then processes the dedicated
     * short-tail reverb instance (weight == impactAmount01).
     * `elapsedSamplesAtSegmentStart`/`totalWindowSamples` describe this
     * segment's position within the latched Impact-state window (same
     * convention as VacuumModule::process()) - `totalWindowSamples` is
     * accepted for API symmetry/future use but not consulted directly:
     * every envelope here is self-terminating from its OWN much-shorter
     * duration.
     *
     * Low-end safety design (Stage 3 DSP Phase 3.4 bugfix): a one-pole
     * crossover extracts the low band's level, but the safety is applied as
     * a single broadband GAIN derived from that level (tanh(low)/low,
     * always <= 1) and multiplied across the WHOLE summed sample - NOT by
     * reconstructing low(clipped)+high(unclipped), which measurably failed
     * to bound the result (a one-pole crossover is not phase-neutral near
     * its cutoff, so the unclipped high residual could itself carry enough
     * of the source's amplitude, 90-ish degrees out of phase, to add back
     * above the original level). The gain-multiplication approach
     * guarantees |output| <= |summed| by construction, plus a final hard
     * clamp as a defensive absolute ceiling.
     *
     * POST-1.0 BUGFIX (2026-09-16, user-directed improvement, see
     * plugins/DROP/CHANGELOG.md - Bug A "instant cut"): PluginProcessor now
     * continues calling process() through the Recovery state too (not
     * state-gated to Impact only), with `elapsedSamplesAtSegmentStart`
     * measured continuously from Impact's own entry (never reset at the
     * Impact->Recovery boundary) so the sub-hit/burst envelopes (which are
     * pure functions of elapsed time, self-terminating well within ~140ms)
     * and - critically - the dedicated `shortTailReverb`'s own algorithmic
     * decay are never abruptly cut off. `reverbSendWeightOverride` (new,
     * defaulted to preserve the exact prior 4-argument call signature/
     * behavior for the Impact state and every existing test call site) lets
     * the caller silence the shortTailReverb's INPUT independently of
     * `impactAmount01` - PluginProcessor passes 0.0f during the Recovery
     * carryover (so the live pass-through audio arriving during Recovery,
     * which by then has nothing to do with the drop, never re-excites the
     * tail) while `impactAmount01` itself is passed already scaled by the
     * tail envelope (so the ALREADY-excited reverb's output rings out
     * smoothly to 0 by Recovery's end, governed by the `tail` parameter).
     */
    /*
     * POST-1.0 (2026-09-17, user-directed - Dark Techno "adds a drum kick"):
     * `accentLevel01` (Style-authored, DropStyleData::impactAccentLevel,
     * default 1.0 == exact prior behavior for every existing call site)
     * scales ONLY the synthesized sub-hit + burst accents. The low-end
     * soft-clip safety and shortTailReverb still run unconditionally on the
     * bus content, so an accent-free Style keeps the identical impact
     * reverb/safety character.
     */
    void process(juce::dsp::AudioBlock<float> block, double elapsedSamplesAtSegmentStart,
                 double totalWindowSamples, float impactAmount01,
                 float reverbSendWeightOverride = -1.0f,
                 float accentLevel01 = 1.0f) noexcept;

    /**
     * Isolated sub-hit-ONLY renderer (sine + AR envelope + bandpass; no
     * burst/soft-clip/reverb), exposed for unit-testing spectral
     * concentration in the 40-80Hz band (plan.md Phase 3.4 test criteria)
     * via the SAME per-sample code path process() uses internally
     * (generateSubHitSample()) - no risk of the tested path drifting from
     * the real path. Mutates the same instance state as process()
     * (subPhase/subBandpass) - call on a freshly prepare()d/reset()
     * instance dedicated to testing, not interleaved with real process()
     * calls.
     */
    void renderSubHitOnly(float* monoOut, int numSamples, double elapsedSamplesAtSegmentStart, float impactAmount01) noexcept;

    /** Pure AR envelope evaluation for the sub-hit (public for testability, mirrors VacuumModule::computeGain()'s convention). */
    static float computeSubEnvelope(double elapsedSamples, double sampleRate) noexcept;

    /** Pure envelope evaluation for the noise burst. */
    static float computeBurstEnvelope(double elapsedSamples, double sampleRate) noexcept;

    static constexpr uint32_t kSeedXorConstant = 0xB5297A4Du; // arbitrary, documented, distinct from NoiseModule's 0x9E3779B9

private:
    float generateSubHitSample(double elapsedSamples, float impactAmount01) noexcept;
    float nextBurstNoise() noexcept;

    double sampleRate = 44100.0;

    // Sub-hit oscillator (raw phase accumulator, architecture.md: "a raw
    // phase accumulator is simpler and avoids an extra dependency for one
    // sine burst" - NOT juce::dsp::Oscillator). Mono generation (added
    // identically to every channel below - LOW-FREQUENCY PROTECTION avoids
    // any phase risk at 40-80Hz).
    float subPhase = 0.0f;
    float subPhaseIncrement = 0.0f;
    juce::dsp::IIR::Filter<float> subBandpass; // band-limits the enveloped sine to ~40-80Hz

    // Noise/transient burst (mono, deterministic).
    std::mt19937 burstRng;
    juce::dsp::IIR::Filter<float> burstBandpass;

    // Low-end soft-clip safety: one-pole crossover (same coefficient
    // pattern as VacuumModule/WidthModule) tracks the LOW band's level per
    // channel, driving a broadband gain applied to the whole sample - see
    // process()'s doc comment for why this is a gain-multiply, not a
    // clipped-low/unclipped-high reconstruction.
    float lowClipState[2] = { 0.0f, 0.0f };
    float lowClipCoeff = 0.0f;

    // Dedicated short-tail reverb instance (architecture.md ReverbModule
    // config: "A second, shorter-decay juce::dsp::Reverb instance... used
    // for the Impact-state short tail" - SAME class, separate instance).
    ReverbModule shortTailReverb;

    static constexpr float kSubFrequencyHz = 60.0f;      // within architecture.md's 40-80Hz target band
    static constexpr float kSubBandpassQ = 1.5f;
    static constexpr double kSubAttackSeconds = 0.0015;  // ~1.5ms fast attack
    static constexpr double kSubDecaySeconds = 0.12;     // ~120ms short decay
    static constexpr float kSubLevel = 0.9f;             // linear amplitude placeholder (Style-authored in Phase 3.5)

    static constexpr float kBurstFrequencyHz = 2200.0f;  // filtered-noise "snap" character, well above the sub band
    static constexpr float kBurstBandpassQ = 0.7f;
    static constexpr double kBurstDurationSeconds = 0.02; // 20ms (architecture.md "10-30ms")
    static constexpr float kBurstLevel = 0.45f;

    static constexpr float kSaturationOverlayScale = 0.35f; // "light" overlay ceiling at impact == 100%
    static constexpr double kDriveAttackFraction = 0.08;    // fast attack over the window's first 8%

    static constexpr float kLowClipCrossoverHz = 120.0f; // architecture.md's protected-frequency convention

    static constexpr float kShortTailRoomSize = 0.18f;   // small room -> short tail
    static constexpr float kShortTailDamping = 0.7f;     // high damping -> "avoid... an indistinct wash"
};

} // namespace drop
