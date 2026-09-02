#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include "dsp/ModulationAccumulator.h"

class CORRUPTRAudioProcessor : public juce::AudioProcessor
{
public:
    CORRUPTRAudioProcessor();
    ~CORRUPTRAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "CORRUPTR"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS() { return parameters; }

private:
    //=========================================================================
    // Stage 2 Phase 3.1 (built) / Phase 3.4 (integrated): Feedback Routing
    // Safety Validation + Integration
    //
    // See architecture.md component #7 ("Feedback Routing Path"), plan.md's
    // "Phase 3.1: Feedback Routing Safety Validation (Isolated)", and
    // plan.md's "Phase 3.4: Feedback Routing Integration". This is the
    // single highest-risk component in CORRUPTR (potential for
    // runaway/audible-harm if the safety math is wrong) and was
    // deliberately built + proven in isolation in Phase 3.1, BEFORE the
    // Distortion Engine (#2) and Filter Stage (#6) existed to be spliced
    // into the loop. As of Phase 3.4, the loop is now wired directly into
    // the live per-sample chain in processBlock() — real Distortion Engine
    // output feeds the loop's input (via the delay-line's recirculated
    // sample summing into the Distortion stage's input), and the Filter
    // Stage's real output is what gets tapped (post-Filter, per the
    // Sequential DSP chain's explicit step-10 ordering — see the
    // tap-point contradiction resolution comment in processBlock()). The
    // safety math below is unchanged and remains unconditional.
    //
    // Safety design (non-negotiable per architecture.md):
    //   1. Soft-clamp gain: internalGain = kFeedbackMaxSafeGain * tanh(feedbackAmount/100)
    //      -> tanh() is bounded in [-1,1] for ALL finite inputs, so the
    //      *parameter itself* can never command unity/runaway gain.
    //   2. In-loop one-pole lowpass damping (feedbackDamping -> cutoff Hz)
    //      attenuates HF energy every pass around the loop.
    //   3. Per-sample std::isfinite() circuit breaker - hard-resets the
    //      delay line + filter + RMS state for that channel if a NaN/Inf
    //      is ever produced (checked every sample, not just once per block,
    //      since a bad sample must never be allowed to recirculate even for
    //      the remainder of the current block - unlike a feedforward chain,
    //      a feedback loop can sustain a bad sample indefinitely).
    //   4. Recommended belt-and-suspenders addition (architecture.md):
    //      continuous RMS-envelope limiter on the feedback path itself,
    //      independent of the tanh gain clamp and of the main Output
    //      Limiter (component #13, built in Phase 3.3) — the feedback tap
    //      sits upstream of Master Mix/Output Limiter, so this RMS limiter
    //      is the only safety net protecting the recirculating loop itself.
    //=========================================================================
    static constexpr float kFeedbackMaxSafeGain = 0.85f;      // architecture.md "Feedback Safety Soft-Clamp" — constant well below 1.0
    static constexpr float kFeedbackDampingMinHz = 200.0f;    // feedbackDamping = 100% -> heaviest HF cut
    static constexpr float kFeedbackDampingMaxHz = 18000.0f;  // feedbackDamping = 0%   -> near-transparent
    static constexpr float kFeedbackRmsLimitThreshold = 0.95f;
    static constexpr float kFeedbackRmsTimeConstantMs = 50.0f;
    static constexpr double kFeedbackDelayHeadroomSeconds = 0.060; // microDelayTime max is 50ms; allocate to 60ms for automation-sweep headroom

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> feedbackDelayLine { 1 };
    std::array<juce::dsp::FirstOrderTPTFilter<float>, 2> feedbackDampingFilter;
    std::array<float, 2> feedbackRmsEnvelope { 0.0f, 0.0f };
    float feedbackRmsOnePoleCoeff = 0.0f;
    int feedbackMaxDelaySamples = 0;
    juce::SmoothedValue<float> feedbackInternalGainSmoothed;
    juce::SmoothedValue<float> feedbackDampingCutoffSmoothed;
    std::atomic<bool> feedbackCircuitBreakerTripped { false }; // diagnostic only (not read for control flow on the audio thread)

    // Real-time-safe: bounded loop over the already-allocated delay buffer
    // (feedbackMaxDelaySamples iterations, no allocation) - the circuit
    // breaker's "hard reset to zero for that channel" mechanism.
    void resetFeedbackLoopChannel(int channel);

    //=========================================================================
    // Stage 2 Phase 3.2: Unified Modulation Accumulator (Isolated)
    //
    // See dsp/ModulationAccumulator.h for the generic, reusable combine-math
    // class (Performance Trigger hard-override, else additive Sequencer +
    // Mod Matrix + Macro combine, always clamped to the destination's valid
    // range - architecture.md's recommended default rule). This section
    // wires that class up to ONE destination only (`drive`, 0-40dB) per
    // plan.md's recommended build order, using SYNTHETIC stand-ins for the
    // Sequencer and Mod Matrix contributions (neither subsystem exists yet -
    // built in Phase 3.7/3.8) and the REAL `macroDamage` APVTS parameter for
    // the Macro contribution (macros are genuine Stage 1 parameters already).
    //
    // Deliberately NOT wired into the live audio signal path yet - this
    // phase proves the accumulator's math in isolation, matching Phase
    // 3.1's "isolated" pattern. The Distortion Engine (architecture.md
    // component #2, which would actually READ a modulated `drive` value)
    // doesn't exist yet either (built in Phase 3.3), so there is nothing
    // for this observation point to feed even if it wanted to. Computed
    // once per block (diagnostic/future-use only, not consumed anywhere
    // yet) and stored in `phase32DriveModulationObservation` below.
    //=========================================================================
    std::atomic<bool> modulationAccumulatorSelfTestPassed { false }; // set once in the constructor (see .cpp)
    std::atomic<float> phase32DriveModulationObservation { 0.0f };   // diagnostic only - last computed modulated `drive` value, not yet applied to any DSP
    double phase32SyntheticSequencerPhase = 0.0;  // free-running phase for the synthetic Sequencer-lane stand-in (advances once per block)
    double phase32SyntheticModMatrixPhase = 0.0;  // free-running phase for the synthetic Mod-Matrix-slot stand-in (advances once per block, different rate than the above so the two are distinguishable)

    //=========================================================================
    // Stage 2 Phase 3.3: Core Linear Chain
    //
    // Working, non-modulated core signal path: Input Gain -> Distortion
    // Engine (12 algorithms) -> Bitcrusher/SRR -> Filter Stage (7
    // topologies) -> Master Mix (0-200%) -> Output Gain -> Output Limiter
    // (dual-mode). See architecture.md components #1, #2, #3, #6, #13, #14
    // and the "Sequential DSP chain (per block, REQUIRED order)" section
    // (steps 3, 5, 6, 9, 11, 12, 13). Oversampling, Glitch, Sequencer, Mod
    // Matrix, and Macros are explicitly OUT of scope for this phase —
    // native sample rate only, no oversampling wrapper yet. As of Phase
    // 3.4, the Feedback Routing Path (Phase 3.1) IS wired into this chain
    // (step 10's tap/recirculation, in processBlock()'s merged per-sample
    // loop) — only Phase 3.2's modulation-accumulator observation code
    // above remains untouched/unwired (that generalization is Phase
    // 3.7-3.9's job).
    //
    // Real-time-safety note: the tone-tilt filter, DC blocker, and Notch
    // filter below are all HAND-ROLLED one-pole/biquad implementations
    // (NOT juce::dsp::IIR::Filter) specifically because
    // IIR::Coefficients<float>::make*() factory functions allocate a new
    // heap-backed Coefficients object on every call — recomputing them
    // every block (needed since filterCutoff/filterResonance/tone/bias are
    // all automatable) would violate "no allocation in processBlock()".
    // Manual coefficient math (plain floats, no JUCE Coefficients object)
    // sidesteps this entirely. StateVariableTPTFilter and DelayLine are
    // used as-is for the other topologies since their setCutoffFrequency/
    // setResonance/setDelay calls are documented as allocation-free.
    //=========================================================================

    // --- Component #1: Input Gain ---
    juce::dsp::Gain<float> inputGainDsp;

    // --- Component #2: Distortion Engine (12-algorithm waveshaper) ---
    std::array<float, 2> distortionToneLpState { 0.0f, 0.0f };  // one-pole LP state for tone-tilt filter (per channel)
    std::array<float, 2> distortionDcBlockerX1 { 0.0f, 0.0f };  // DC blocker x[n-1] (per channel)
    std::array<float, 2> distortionDcBlockerY1 { 0.0f, 0.0f };  // DC blocker y[n-1] (per channel)
    std::array<juce::dsp::Oscillator<float>, 2> ringModOsc;     // Ring-Mod carrier (per channel, sine, lookup-table mode)
    float distortionToneLpCoeff = 0.0f; // computed in prepareToPlay from a fixed 1kHz tilt-filter pole
    float distortionDcBlockerR = 0.0f;  // computed in prepareToPlay from a fixed ~5Hz DC-blocker pole
    juce::SmoothedValue<float> driveGainSmoothed;     // smoothed linear gain from `drive` (dB) — avoids zipper on automation
    juce::SmoothedValue<float> distortionMixSmoothed; // smoothed 0-1 stage-local dry/wet (`distortionMix`)

    // Per-block parameter cache (recomputed every block from APVTS, not
    // persistent DSP state — plain scalars, no allocation).
    int distortionAlgorithmIndex = 0;
    bool distortionEngineBypassed = false; // graphBypassSaturation (whole-module power-bypass)
    bool waveshaperCurveBypassed = false;  // graphBypassWaveshaper ("Fold Bypass" — waveshaper-curve-only bypass)
    float distortionBiasOffset = 0.0f;
    float distortionToneNorm = 0.0f;
    float distortionFoldPct = 0.0f;

    float processDistortionEngine(float xIn, int channel, float driveGain, float distortionMixAmt);

    // --- Component #3: Bitcrusher / Sample Rate Reducer (always-on) ---
    std::array<float, 2> bitcrushHeldSample { 0.0f, 0.0f };
    std::array<int, 2> bitcrushHoldCounter { 0, 0 };
    bool bitcrushBypassed = false;
    float bitcrushLevels = 65535.0f;
    int bitcrushHoldSamples = 1;

    float processBitcrusher(float xIn, int channel);

    //=========================================================================
    // Stage 2 Phase 3.5: Glitch / Buffer Engine (18 Modes) — Tier 1 (Simple
    // Modes) — architecture.md component #4, plan.md's "Phase 3.5: Glitch
    // Engine - Tier 1 (Simple Modes)".
    //
    // Positioned between Bitcrusher/SRR (#3) and Filter Stage (#6) in the
    // per-sample chain, per the Processing Chain ASCII diagram and
    // Sequential DSP chain step 8 (Glitch runs on Bitcrusher's OUTPUT,
    // feeds the Filter Stage's INPUT — the Oversampling bracket around
    // Distortion+Bitcrush is not built yet, per Phase 3.3/3.4's scope, so
    // Glitch simply follows Bitcrusher's real output directly).
    //
    // Scope: EXACTLY 6 Tier-1 modes per plan.md's explicit list — Stutter
    // (glitchMode index 1), Repeat (2), Reverse (3), Freeze (5), Retrigger
    // (12), Silence (13). All other glitchMode selections (0/Off, and the
    // Tier-2 modes 4, 6-11, 14-17) are a clean dry passthrough this phase —
    // `isTier1PlayableMode` in setUpGlitchEventForThisCycle()'s call site
    // gates this: an event can only ever go "active" (mixAmt ramps above 0)
    // for one of the 6 Tier-1 indices, so every other index's output is
    // architecturally guaranteed to equal its dry input, not a
    // half-implemented stub.
    //
    // Ring buffer: `glitchRingBuffer` (juce::AudioBuffer<float>, stereo,
    // allocated ONCE in prepareToPlay, NEVER resized/reallocated on the
    // audio thread) is written continuously with the incoming (post-
    // Bitcrush) signal EVERY sample, regardless of whether a glitch is
    // currently active or graphBypassGlitch is on — architecture.md: "so
    // there's always a recent-history window available to capture from
    // when a trigger fires." Sized for "4 bars at slowest supported tempo"
    // (architecture.md's own example, 40bpm) at the ACTUAL prepared sample
    // rate (see prepareToPlay), not a hardcoded 192kHz — this is correctly
    // generous at whatever real sample rate the host uses while still
    // covering the worst-case (slowest-tempo, longest-division) capture
    // window that rate could ever need.
    //
    // Trigger mechanism: a free-running per-sample cycle counter
    // (`glitchCycleSampleCounter`) whose period is derived from
    // `glitchBufferLength` (1/64..4 bars) converted to samples via host BPM
    // (`juce::AudioPlayHead::getPosition()->getBpm()`, JUCE 8
    // std::optional API; falls back to `kGlitchFallbackBpm` = 120 if no
    // host/no tempo info, real-time-safe - no allocation in that fallback
    // path). At every cycle boundary a Bernoulli trial (seeded
    // `glitchRandom`, gated by `glitchProbability`) decides whether a
    // glitch event fires this cycle. NOT phase-locked to the host's
    // bar/beat grid (a deliberate scope simplification - architecture.md
    // only specifies BPM-derived sample-count conversion, not PPQ
    // phase-locking; documented in the JSON report as a reasonable
    // interpretation).
    //
    // Chaos scaling (architecture.md: "chaos scales probability variance
    // and adds controlled random jitter to buffer window
    // selection/length"), exact formula:
    //   chaosNorm = chaos% / 100
    //   probJitter   = (2*rand()-1) * chaosNorm * kGlitchChaosProbabilityJitterRange   (+/-30% of probability at chaos=100%)
    //   lengthJitter = (2*rand()-1) * chaosNorm * kGlitchChaosWindowLengthJitterRange  (+/-50% of cycle length at chaos=100%)
    //   effectiveProbability = clamp01(glitchProbability% / 100 + probJitter)
    //   thisCycleLength = clamp(nominalCycleLengthSamples * (1 + lengthJitter), 1, ringBufferCapacity - headroom)
    // Both jitter draws happen from the seeded `glitchRandom` instance
    // EVERY cycle boundary, unconditionally (regardless of `glitchMode`/
    // `graphBypassGlitch`), so the RNG draw sequence — and therefore
    // reproducibility — depends only on tempo/sample-rate/probability/
    // chaos, never on which mode happens to be selected or whether the
    // module is bypassed at any given moment.
    //
    // RNG seeding (architecture.md: "must use a seeded juce::Random
    // instance (not the shared global RNG)... seed stored in custom
    // state"): `glitchRandom` is a private, non-global `juce::Random`
    // member, reseeded ONLY from `glitchRandomSeedAtomic` (never from
    // `juce::Random::getSystemRandom()` after construction). The atomic is
    // written from the message thread (constructor's one-time initial
    // seed draw; `setStateInformation`'s preset-restore path) and read
    // once per BLOCK on the audio thread (architecture.md Thread
    // Boundaries: "read once per relevant trigger event, not per-sample"),
    // reseeding the real `glitchRandom` instance only when the atomic
    // value actually changed since the last block. See
    // getStateInformation/setStateInformation for the persistence
    // mechanism (a plain XML attribute on the APVTS state's root element,
    // not a second nested ValueTree — sufficient for a single int64 at
    // this phase's scope; Sequencer pattern data in Phase 3.7 may
    // introduce the fuller nested-ValueTree pattern architecture.md
    // describes).
    //
    // Per-mode window semantics (architecture.md's per-mode notes):
    //   Stutter (1):   short slice (kGlitchStutterFraction of the cycle
    //                  length), captured ONCE at trigger onset, looped
    //                  forward for the rest of the cycle.
    //   Repeat (2):    the FULL captured cycle-length window, captured
    //                  once, looped forward.
    //   Reverse (3):   same as Repeat but read back-to-front every pass
    //                  (`glitchReverseDirection`).
    //   Freeze (5):    a very short (`kGlitchFreezeWindowMs`, "~single-
    //                  cycle" approximation per architecture.md), captured
    //                  once, looped forward — the classic hard-freeze
    //                  texture.
    //   Retrigger (12): short slice (kGlitchRetriggerFraction), but UNLIKE
    //                  Stutter/Repeat, RE-CAPTURED fresh from the current
    //                  ring-buffer write position at the START OF EVERY
    //                  LOOP PASS (see readGlitchWindowSample) rather than
    //                  held fixed for the whole event — a deliberate
    //                  design differentiation (architecture.md groups
    //                  Stutter/Repeat/Retrigger/Micro Loop together without
    //                  specifying what distinguishes them) giving Retrigger
    //                  a live, rhythmically-gated re-sync character instead
    //                  of Stutter/Repeat's fixed-loop character. Documented
    //                  in the JSON report.
    //   Silence (13):  hard/soft-gated mute — `readGlitchWindowSample`
    //                  returns 0.0f unconditionally; the crossfade envelope
    //                  below still provides click-free fade in/out.
    //
    // Click-free guarantees (plan.md Phase 3.5 Test Criteria):
    //   1. Window entry/exit: `glitchActiveMixSmoothed` (shared
    //      SmoothedValue, kGlitchCrossfadeMs ramp) blends dry<->wet at
    //      every event start/end — advanced once per sample in the outer
    //      loop, matching this file's established convention for
    //      channel-shared SmoothedValues.
    //   2. Loop-back discontinuities (Stutter/Repeat/Reverse/Freeze/
    //      Retrigger's internal loop points): `readGlitchWindowSample`
    //      applies a linear fade-in/fade-out of `glitchLoopEdgeFadeSamples`
    //      at the START and END of EVERY pass through the window (not just
    //      the first), so the read pointer's wrap-to-start is never a hard
    //      discontinuity, regardless of how many times it loops.
    //   3. Mode switching mid-playback: detected once per block (matching
    //      this file's established `lastFilterTypeIndex`-style change-
    //      detection convention) — forces `glitchEventActiveThisCycle =
    //      false` and a fresh cycle-boundary recompute, which drives
    //      `glitchActiveMixSmoothed`'s target to 0 and ramps cleanly to dry
    //      via the same mechanism as #1, even if a window was mid-playback
    //      at the moment of the mode change.
    //
    // Real-time safety: `glitchRingBuffer` allocated once in
    // prepareToPlay, sized generously, NEVER resized in processBlock().
    // `glitchMinWindowSamples`/`glitchFreezeWindowSamples`/
    // `glitchLoopEdgeFadeSamples` are precomputed (int, from sample rate)
    // in prepareToPlay so no float->int conversion of a sample-rate-
    // derived constant happens per-sample. All per-sample RNG draws
    // (`juce::Random::nextFloat()`) are allocation-free per JUCE's
    // documented implementation. `AudioPlayHead::getPosition()` is called
    // once per BLOCK (not per-sample) and returns a stack-based
    // `std::optional`, no heap allocation.
    //=========================================================================
    static constexpr double kGlitchMinSupportedBpm = 40.0;                // architecture.md's own worst-case bpm example, used for ring-buffer sizing headroom
    static constexpr float  kGlitchFallbackBpm = 120.0f;                  // no-host/no-tempo-info fallback
    static constexpr float  kGlitchCrossfadeMs = 4.0f;                    // window-entry/exit + mode-switch crossfade
    static constexpr float  kGlitchLoopEdgeFadeMs = 3.0f;                 // per-loop-pass click guard at window start/end
    static constexpr float  kGlitchFreezeWindowMs = 15.0f;                // "~single-cycle" Freeze window approximation
    static constexpr float  kGlitchStutterFraction = 0.125f;              // Stutter window = 1/8 of the nominal division length
    static constexpr float  kGlitchRetriggerFraction = 0.25f;             // Retrigger window = 1/4 of the nominal division length
    static constexpr float  kGlitchChaosProbabilityJitterRange = 0.3f;    // +/-30% probability swing at chaos=100%
    static constexpr float  kGlitchChaosWindowLengthJitterRange = 0.5f;   // +/-50% cycle-length swing at chaos=100%
    static constexpr std::array<float, 9> kGlitchDivisionMultiplier {     // index = glitchBufferLength choice (1/64..4 bars), value = fraction/multiple of one whole-note ("1 bar", 4/4 assumption)
        1.0f / 64.0f, 1.0f / 32.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f, 1.0f, 2.0f, 4.0f
    };

    juce::AudioBuffer<float> glitchRingBuffer; // stereo history buffer, allocated once in prepareToPlay
    int glitchRingBufferLength = 0;            // capacity in samples (per channel)
    int glitchRingWritePos = 0;                // shared write index (both channels write at the same index each sample)

    int glitchCycleSampleCounter = 0;          // free-running countdown, 0..glitchCurrentCycleLengthSamples-1
    int glitchCurrentCycleLengthSamples = 0;   // recomputed (with chaos jitter) at each cycle boundary
    bool glitchEventActiveThisCycle = false;   // this cycle's Bernoulli-trial result (gated by isTier1PlayableMode)

    int glitchActiveWindowLengthSamples = 0;   // length of the currently-playing captured window (shared, both channels read the same historical window)
    int glitchWindowReadStartIndex = 0;        // ring-buffer index where the captured window begins (shared)
    bool glitchReverseDirection = false;       // Reverse mode: read the window back-to-front
    std::array<int, 2> glitchWindowReadPos { 0, 0 }; // per-channel offset within the window (kept per-channel defensively; both channels always advance in lockstep)

    int glitchMinWindowSamples = 8;            // sample-rate-derived floor (prepareToPlay), avoids degenerate near-zero windows
    int glitchFreezeWindowSamples = 0;         // sample-rate-derived Freeze window length (prepareToPlay)
    int glitchLoopEdgeFadeSamples = 0;         // sample-rate-derived per-loop-pass click-guard fade length (prepareToPlay)

    juce::Random glitchRandom;                                    // seeded, NOT juce::Random::getSystemRandom() — architecture.md requirement
    std::atomic<juce::int64> glitchRandomSeedAtomic { 0 };         // authoritative seed (message-thread writes: ctor + setStateInformation; audio-thread reads once/block)
    juce::int64 glitchLastAppliedRandomSeed = 0;                   // audio-thread-only shadow, detects atomic seed changes

    juce::SmoothedValue<float> glitchActiveMixSmoothed; // 0 = dry passthrough at this stage, 1 = glitch wet output
    int lastGlitchModeIndex = -1;                        // detects glitchMode parameter changes -> forces a crossfade-to-dry release

    bool glitchBypassed = false;         // graphBypassGlitch
    int glitchModeIndex = 0;             // glitchMode
    int glitchBufferLengthIndex = 2;     // glitchBufferLength
    float glitchProbabilityPct = 0.0f;   // glitchProbability
    float glitchChaosPct = 0.0f;         // chaos

    float processGlitchEngine(float xIn, int channel, int writePos, float mixAmt);
    void setUpGlitchEventForThisCycle(int writePosThisSample);
    float readGlitchWindowSample(int channel);

    // --- Component #6: Filter Stage (7 topologies) ---
    std::array<juce::dsp::StateVariableTPTFilter<float>, 2> svfFilter; // LP/HP/BP/Resonant LP/Resonant HP
    std::array<float, 2> notchX1 { 0.0f, 0.0f }, notchX2 { 0.0f, 0.0f };
    std::array<float, 2> notchY1 { 0.0f, 0.0f }, notchY2 { 0.0f, 0.0f };
    float notchB0 = 1.0f, notchB1 = 0.0f, notchB2 = 0.0f, notchA1 = 0.0f, notchA2 = 0.0f; // manual biquad coeffs (see rationale above)
    std::array<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>, 2> combDelay;
    int combMaxDelaySamples = 0;
    float combFeedbackGain = 0.0f;
    int lastFilterTypeIndex = -1; // triggers a one-time state reset when filterType changes (architecture.md: "must reset state on filterType change")
    bool filterBypassed = false;
    int filterTypeIndex = 0;

    void updateFilterParameters(float cutoffHz, float resonancePct, double sampleRate);
    float processFilterStage(float xIn, int channel);

    // --- Component #14: Master Mix ---
    juce::AudioBuffer<float> dryBuffer; // captured at the very start of processBlock(), before Input Gain
    juce::SmoothedValue<float> masterDryGainSmoothed;
    juce::SmoothedValue<float> masterWetGainSmoothed;

    // --- Component #13: Output Gain + Output Limiter (dual-mode) ---
    // CONTRADICTION FLAGGED & RESOLVED (see JSON report): architecture.md's
    // component #13 prose says "outputGain applied after the limiter", but
    // its own "Sequential DSP chain (REQUIRED order)" section lists step 12
    // Output Gain BEFORE step 13 Output Limiter. This implementation follows
    // the Sequential DSP chain's explicit numbered order (authoritative per
    // its own text, "REQUIRED order") — Output Gain, THEN Output Limiter.
    juce::dsp::Gain<float> outputGainDsp;
    juce::dsp::WaveShaper<float> coloredLimiterSaturation; // fixed, gentle, always-on-in-Colored-mode saturation before the limiter (architecture.md #13's recommended MVP approximation for GR-proportional coloring)
    juce::dsp::Limiter<float> outputLimiter;

    juce::AudioProcessorValueTreeState parameters;

    // Parameter layout creation
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessor)
};
