#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>

/**
 * PitchModule - granular/SOLA-style time-domain pitch shifter.
 *
 * See plugins/DROP/.ideas/architecture.md > Core Components > PitchModule
 * and its "PitchModule - Deep Dive" Algorithm Details section (HIGH-risk
 * component; mitigation strategy: "Implement and unit-test the grain engine
 * in isolation... before wiring it into PitchModule/StutterModule" - see
 * GranularGrainEngine below, and Source/Tests/RunTests.cpp's isolated
 * FFT-based semitone-verification test).
 * Also see plugins/DROP/.ideas/implementation-spec.md > PITCHMODULE.
 *
 * Architecture choice (architecture.md "Pitch Shifting Algorithm Choice"):
 * synchronous overlap-add (SOLA), NOT an FFT phase vocoder - quantized
 * musical semitone steps + "prefer zero-latency MVP" favor this simpler,
 * lower-latency, lower-CPU approach.
 *
 * GRAIN COUNT (post-3.3-review correction): an earlier version of this
 * engine ran 4 SIMULTANEOUSLY-OVERLAPPING Hann-windowed grains at 75%
 * overlap (the architecture doc's literal "4x overlap" suggestion). That
 * design was analytically proven (during the Phase 3.3 review) to suffer
 * unavoidable destructive phase interference for tonal content: two grains
 * whose retriggers are offset by `hop` real-time samples produce outputs at
 * the shifted frequency r*f0 with a relative phase of
 * `2*pi*hop*(1-r)/T0` (T0 = the INPUT's period in samples) - a function of
 * the input's unknown pitch, which a blind (non-pitch-synchronous) grain
 * engine has no way to align to. For a pure 440Hz test tone this measured
 * as ~-145 degrees (strongly destructive) at +2 semitones, causing the
 * isolated FFT test to measure a dominant frequency far from the requested
 * ratio - not a simple arithmetic bug, a genuine limitation of 4-way blind
 * overlap for periodic material (architecture.md's own Implementation
 * Risks section anticipates exactly this: "preserving phase-continuity
 * heuristically... when feasible, or simply a fixed grain-length re-trigger
 * for a first-pass implementation" - the 4-grain version was at that
 * "first-pass" fallback stage and it doesn't hold up against a pure-tone
 * worst case).
 *
 * FIX (round 2): 2-voice continuous-read equal-power crossfade (same proven
 * pattern as StutterModule's dual-voice looper). Only ONE voice is active
 * at full gain outside a short (~15% of grain length) crossfade transition,
 * so the steady-state majority of the output is a SINGLE, uninterfered
 * rate-r read - there is no N-way simultaneous-grain interference to cancel
 * the shifted carrier. The underlying causal delay-amount formula (see
 * below) is unchanged and still provably safe; only the NUMBER of
 * simultaneously-summed voices (and how/when they retrigger) changed.
 *
 * FIX (round 3) - pitch-synchronous retriggering: round 2 reduced but did
 * NOT eliminate phase interference - a crossfade still recurs every
 * (grainLengthSamples - crossfadeLength) samples, and at each crossfade the
 * content-position offset between the outgoing and incoming voice is
 * exactly `(grainLengthSamples-crossfadeLength)*(1-r)` samples, giving a
 * relative phase of `360*offset/T0` degrees against the (unknown) input
 * period T0 - analytically proven to wrap through many full cycles across
 * the supported ratio range regardless of grain/crossfade length choice
 * (no fixed-geometry blind retrigger schedule can avoid this for arbitrary
 * T0). The fix: a bounded-lag normalized autocorrelation (computeSyncOffset(),
 * run ONCE per crossfade event, not per-sample) estimates T0 from the
 * actual input, then a small search (+/- T0_est/2) around the nominal
 * `r*grainLengthSamples` retrigger offset finds the delay that best
 * phase-matches the outgoing voice's current tail, snapping to it. This
 * directly targets the offset causing interference rather than tuning
 * geometry blindly (architecture.md: "choosing the re-trigger offset that
 * best matches waveform periodicity when feasible" - the ideal this module
 * always deferred to a fallback for). A reliability gate (normalized
 * correlation below threshold, or near-silent analysis/tail windows) falls
 * back to the nominal (unsynchronized, round-2) offset, so noise/transient/
 * silent material behaves identically to before - synchronization only
 * engages when a confident periodicity estimate exists. Deterministic
 * (input-driven, no RNG); fixed-cost/no-allocation (preallocated scratch
 * windows, bounded lag/search ranges) - see computeSyncOffset()'s .cpp
 * comment for the exact bound.
 */
namespace drop
{

/**
 * GranularGrainEngine - the reusable grain-engine "building block" behind
 * PitchModule's Rise/Fall/Rise+Fall musical pitch movement AND
 * StutterModule's "pitch variation" repeat mode (architecture.md
 * StutterModule config: "pitch variation... reusing PitchModule's grain
 * engine for the 'pitch variation' mode" / Integration Points: "PitchModule
 * -> StutterModule (optional): Stutter's 'pitch variation' mode reuses
 * PitchModule's grain engine rather than duplicating it."). Declared here
 * (not duplicated in StutterModule.h) so both owners share the exact same
 * algorithm/class; StutterModule owns its OWN instance (its own delay-line
 * buffer, sized for its own use case) rather than sharing PitchModule's
 * buffer instance directly - sharing the CODE (the point of "not
 * duplicating it"), not forcing an awkward cross-module buffer-ownership
 * dependency between two otherwise-independent always-processing modules.
 *
 * Causal-by-construction grain scheduling: for a fixed per-grain playback
 * ratio `r`, a grain retriggered "now" and read via
 * delayInSamples(age) = r*grainLengthSamples + age*(1-r)
 * always satisfies writePos(age) - readPos(age) >= 0 for every age in
 * [0, grainLengthSamples] and every r in [kMinRatio, kMaxRatio] (proof: the
 * trailing distance simplifies to age*(1-r) + r*grainLengthSamples, which
 * is >= 0 whenever r <= 1 by direct inspection, and for r > 1 is minimized
 * at age = grainLengthSamples where it equals exactly grainLengthSamples >
 * 0) - so a juce::dsp::DelayLine (which can only read the PAST relative to
 * its write pointer) is sufficient; no custom circular buffer/interpolation
 * code is needed. This directly reuses the same Lagrange3rd fractional-delay
 * interpolation approach DelayModule already uses (architecture.md:
 * "matching the interpolation approach already used by DelayModule").
 *
 * IMPORTANT juce::dsp::DelayLine gotcha (found+fixed post-initial-3.3
 * implementation): popSample()'s `delayInSamples` is resolved against the
 * line's own internal per-channel readPos, which only advances when
 * `updateReadPointer` is true - it is NOT automatically "N samples behind
 * whatever was just pushed". For genuine multi-tap reads relative to a
 * consistent "now", exactly ONE of the per-sample taps must be called with
 * `updateReadPointer=true` (advancing readPos once, matching pushSample's
 * own once-per-sample advance); the rest use `false` and read using that
 * same not-yet-advanced readPos. See processChannel()'s .cpp comment.
 *
 * RT-safe: delay line + fixed 2-voice state preallocated in prepare(); no
 * allocation in beginSample()/processChannel().
 */
class GranularGrainEngine
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec, float grainLengthMs);
    void reset();

    /** Target playback-rate ratio (2^(semitones/12)); internally smoothed. */
    void setRatio(float newRatio) noexcept { ratioSmoothed.setTargetValue(juce::jlimit(kMinRatio, kMaxRatio, newRatio)); }

    /**
     * Advances the active voice (and, during a crossfade transition, the
     * other voice too) by exactly one sample and caches this sample's
     * delay-amount(s)/gain(s). Must be called EXACTLY ONCE per output
     * sample, before any processChannel() calls for that sample.
     */
    void beginSample() noexcept;

    /**
     * Pushes `input` into channel `channel`'s delay line and returns the
     * pitch-shifted output for that channel at the CURRENT sample (per the
     * most recent beginSample() call). Call once per channel per sample.
     */
    float processChannel(int channel, float input) noexcept;

    static constexpr float kMinRatio = 0.5f;  // -12 semitones
    static constexpr float kMaxRatio = 2.0f;  // +12 semitones

private:
    struct Voice
    {
        double age = 0.0;              // samples since (re)trigger, in [0, grainLengthSamples]
        float ratioAtRetrigger = 1.0f; // ratio latched for this voice's whole lifetime (per the causality proof above)
        double syncOffset = 0.0;       // pitch-synchronous retrigger adjustment (round 3), latched at trigger time; 0.0 == unsynchronized/nominal (the fallback value)
    };

    /** delayInSamples(age) = r*grainLengthSamples + age*(1-r) + syncOffset - see the causality proof above (syncOffset is a bounded, per-voice-constant adjustment latched at trigger time by computeSyncOffset() - added AFTER the proof's formula, not altering it; the existing clamp to [0,maxDelay] in beginSample() is defense-in-depth against any edge case). */
    double computeDelayAmt(const Voice& v) const noexcept
    {
        return (double) v.ratioAtRetrigger * grainLengthSamples + v.age * (1.0 - (double) v.ratioAtRetrigger) + v.syncOffset;
    }

    /**
     * Pitch-synchronous retrigger search (round 3) - see the class doc
     * comment and the .cpp implementation comment for the full derivation,
     * cost bound, and reliability-gate behavior. Returns the offset (in
     * samples, may be 0.0 = "no adjustment, use nominal") to ADD to
     * `baseline` (the nominal `r*grainLengthSamples` retrigger position) so
     * the new voice's content phase-matches the outgoing voice's current
     * tail (at `activeDelayNow`). Channel-0-only analysis (a deliberate
     * lightweight simplification - see .cpp).
     */
    double computeSyncOffset(double baseline, double activeDelayNow) noexcept;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> delayLine;

    Voice voices[2];
    int activeVoice = 0;
    double crossfadePos = -1.0; // -1 == not currently crossfading (mirrors StutterModule's dual-voice pattern)
    double crossfadeLength = 0.0; // samples, computed in prepare() as ~15% of grainLengthSamples

    // Cached per-sample values (computed in beginSample(), consumed by processChannel()).
    float cachedDelayActive = 0.0f, cachedDelayOther = 0.0f;
    float cachedGainActive = 1.0f, cachedGainOther = 0.0f;
    bool crossfadingThisSample = false;

    juce::SmoothedValue<float> ratioSmoothed;

    double grainLengthSamples = 1323.0; // ~30ms @ 44.1kHz, recomputed in prepare()
    double sampleRate = 44100.0;

    static constexpr double kRatioSmoothingSeconds = 0.005;

    // Pitch-synchronous retrigger search (round 3) - preallocated scratch,
    // bounded lag/window sizes (no allocation, fixed worst-case cost per
    // crossfade event - see computeSyncOffset()'s .cpp comment).
    static constexpr int kSyncAnalysisWindow = 48;   // samples correlated at each candidate lag (period estimation)
    static constexpr int kSyncMinLagSamples = 20;    // ~2400Hz ceiling @48kHz (scales in Hz with sample rate, fixed in samples - a lightweight, documented trade-off)
    static constexpr int kSyncMaxLagSamples = 240;   // ~200Hz floor @48kHz - also bounds the snap-offset search radius (+/- kSyncMaxLagSamples/2)
    static constexpr int kSyncMatchWindow = 24;      // samples correlated at each candidate snap offset
    static constexpr float kSyncMinReliableCorrelation = 0.5f; // reliability gate (normalized correlation, both steps) - below this, fall back to the nominal/unsynchronized offset
    float syncBaseWindow[kSyncAnalysisWindow] {};
    float syncTailWindow[kSyncMatchWindow] {};
};

enum class PitchMode
{
    Rise = 0,
    Fall,
    RiseAndFall
};

class PitchModule
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();

    void setMode(PitchMode newMode) noexcept { mode = newMode; }
    PitchMode getMode() const noexcept { return mode; }

    /**
     * Quantizes `semitones` to the nearest value in architecture.md's
     * musical set (0/+1/+2/+3/+5/+7/+12 and symmetric downward equivalents)
     * and stores it as this build's Rise/Fall/Rise+Fall TARGET semitone
     * value (implementation-spec.md PITCHMODULE: "Musical ranges: 0, +1,
     * +2, +3, +5, +7, +12 semitones and reasonable downward equivalents").
     */
    void setSemitoneTarget(int semitones) noexcept { semitoneTarget = quantizeSemitones(semitones); }
    int getSemitoneTarget() const noexcept { return semitoneTarget; }

    /**
     * Processes `block` in place (weight-mixed against the module's own
     * local dry capture, architecture.md "Always-Process, Weight-Mix").
     * `progress` (0-1) drives the continuous Rise/Fall/Rise+Fall shaping
     * function toward `semitoneTarget` - the TARGET is quantized/musical,
     * the in-between ramp is continuous/transition-controlled
     * (implementation-spec.md: "Pitch amount must be transition-controlled
     * and musically synchronized").
     */
    void process(juce::dsp::AudioBlock<float> block, float weight, float progress) noexcept;

    static float ratioForSemitones(float semitones) noexcept { return std::pow(2.0f, semitones / 12.0f); }
    static int quantizeSemitones(int raw) noexcept;

private:
    static float shapeProgress(PitchMode mode, float progress) noexcept;

    GranularGrainEngine engine;
    juce::AudioBuffer<float> dryBuffer;
    juce::SmoothedValue<float> weightSmoothed;
    PitchMode mode = PitchMode::Rise;
    int semitoneTarget = 12;

    static constexpr float kGrainLengthMs = 30.0f;

    // architecture.md: "never fully replacing the dry signal... at weight
    // 1.0 for Rise/Fall modes below +12" - the Style flag `pitchFullWet`
    // that allows exceptions lands in Phase 3.5; this fixed ceiling is this
    // phase's placeholder.
    static constexpr float kMaxWetAtWeight1 = 0.85f;
};

} // namespace drop
