#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
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
    // Tier-2 modes 4, 6-11, 14-17) were a clean dry passthrough in THIS
    // phase — gated at the trigger call site in processBlock() by a local
    // `isTier1PlayableMode` boolean.
    //
    // UPDATED IN PHASE 3.6: the remaining 11 Tier-2 modes are now also
    // implemented (see the "Stage 2 Phase 3.6" doc comment below, after
    // readGlitchWindowSample()'s declaration). The gate at the trigger call
    // site was renamed `isImplementedMode` and widened to cover all 17
    // non-Off indices (1-17) — Off (0) remains the only mode that never
    // goes "active." Silence (13) is still hard-gated mute inside
    // readGlitchWindowSample(), unchanged from this phase.
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

    //=========================================================================
    // Stage 2 Phase 3.6: Glitch / Buffer Engine (18 Modes) — Tier 2
    // (Advanced Modes) — architecture.md component #4, plan.md's "Phase
    // 3.6: Glitch Engine - Tier 2 (Advanced Modes)".
    //
    // Implements the 11 glitchMode indices Phase 3.5 left as dry
    // passthrough: Buffer Repeat (4), Slice (6), Random Slice (7), Micro
    // Loop (8), Granular Repeat (9), Tape Stop (10), Tape Start (11),
    // Noise Burst (14), Bitcrush Burst (15), Pitch Jump (16), Random
    // Repeat (17) — the exact index set per parameter-spec.md's locked
    // `glitchMode` choice list, cross-checked against architecture.md's
    // per-mode implementation notes. REUSES Phase 3.5's ring buffer
    // (`glitchRingBuffer`), write-position, cycle/trigger bookkeeping,
    // seeded `glitchRandom`, and shared `glitchActiveMixSmoothed` crossfade
    // wholesale — no second ring buffer or duplicate trigger machinery was
    // built. `setUpGlitchEventForThisCycle()`'s mode switch and
    // `readGlitchWindowSample()`'s read logic are EXTENDED in place (not
    // duplicated) to cover these 11 additional indices; readGlitchWindowSample
    // dispatches Tier-2 modes needing bespoke read machinery to a handful of
    // new private helpers (below), while Buffer Repeat (4)/Micro Loop
    // (8)/Bitcrush Burst (15) simply fall through into Tier-1's existing
    // per-channel captured-window loop-read path (they only need a
    // different window LENGTH, or — for Bitcrush Burst — one extra
    // quantize step after that same read, not different read machinery).
    //
    // Per-mode design (fraction/size choices not pinned down numerically by
    // architecture.md — architecture.md only says "replay a captured window
    // repeatedly" for the Stutter/Repeat/Retrigger/Micro Loop/Buffer Repeat
    // group without differentiating window sizes between them; the actual
    // numeric constants below are this implementation's own resolution,
    // chosen so every mode remains audibly distinct from its siblings —
    // documented here per this file's established precedent for flagging
    // such resolutions):
    //   Buffer Repeat (4):  captured ONCE at trigger (fixed, like Stutter/
    //                       Repeat's held-loop character), `kGlitchBufferRepeatFraction`
    //                       (1/2) of the cycle length — a moderate chunk,
    //                       between Stutter's 1/8 and Repeat's full window.
    //   Micro Loop (8):     captured ONCE, `kGlitchMicroLoopFraction` (1/16)
    //                       of the cycle length — finer-grained than
    //                       Stutter, and (unlike Freeze's fixed-ms window)
    //                       TEMPO-RELATIVE, scaling with glitchBufferLength.
    //   Slice (6) /
    //   Random Slice (7):   the FULL captured cycle-length window is
    //                       subdivided into `kGlitchSliceCount` (8) equal
    //                       slices; Slice plays them in natural order,
    //                       looping 0..7,0..7,...; Random Slice shuffles the
    //                       play order ONCE at event trigger (Fisher-Yates,
    //                       seeded `glitchRandom`) then loops that fixed
    //                       shuffled sequence for the rest of the event —
    //                       distinct from re-shuffling every pass, which
    //                       would sound more like continuous randomization
    //                       than a single "sliced" arrangement.
    //   Granular Repeat (9): `kGlitchGranularVoiceCount` (3) fixed,
    //                       round-robin-spawned overlapping grain voices,
    //                       each `kGlitchGrainLengthMs` (45ms, a FIXED
    //                       length independent of glitchBufferLength so the
    //                       Hann window table can be sized/filled exactly
    //                       ONCE in prepareToPlay — a deliberate real-time-
    //                       safety-motivated scope simplification, flagged
    //                       here), windowed via a Hann table baked from
    //                       `juce::dsp::WindowingFunction` (see below for
    //                       why the table is pre-baked into a plain
    //                       std::vector rather than calling the
    //                       WindowingFunction instance per-sample). Each
    //                       grain's start position is a fresh random offset
    //                       within the captured window, drawn from
    //                       `glitchRandom` at the moment that grain spawns
    //                       (an "extra draw at the fired-event path", per
    //                       the established RNG-invariant convention — see
    //                       "RNG draw sequence" note below).
    //   Tape Stop (10) /
    //   Tape Start (11):   see "Tier-2 fractional-rate reads" below.
    //   Pitch Jump (16):   see "Tier-2 fractional-rate reads" below.
    //   Noise Burst (14):  `juce::Random`-generated white noise, drawn
    //                       PER-SAMPLE, PER-CHANNEL independently (from the
    //                       shared `glitchRandom` stream) for
    //                       stereo-decorrelated noise character, scaled by
    //                       a fixed `kGlitchNoiseBurstAmplitude` constant
    //                       (NOT matched to the live input signal's level —
    //                       an MVP simplification, flagged; the ring buffer
    //                       is not read for this mode).
    //   Bitcrush Burst (15): reuses the SAME captured-window loop-read path
    //                       as Buffer Repeat/Repeat (full cycle-length
    //                       window), then applies one extra fixed-depth
    //                       quantize step (`kGlitchBitcrushBurstLevels`,
    //                       ~3-bit) to the already-read sample. Deliberately
    //                       self-contained to the Glitch Engine — does NOT
    //                       read or write Component #3's `bitDepth`/
    //                       `sampleRateReduction` state, per this phase's
    //                       task instructions.
    //   Random Repeat (17): same window SIZE as Buffer Repeat
    //                       (`kGlitchBufferRepeatFraction`), but — unlike
    //                       Buffer Repeat's fixed captured window —
    //                       re-selects a brand-new RANDOM historical
    //                       position (relative to the CURRENT ring-buffer
    //                       write position, so it always stays within
    //                       already-written history) every time the loop
    //                       wraps. This is Random Repeat's differentiator
    //                       from both Buffer Repeat (fixed window) and
    //                       Retrigger (Tier 1 - re-syncs to the CURRENT
    //                       write position specifically, not a random
    //                       historical offset).
    //
    // Tier-2 fractional-rate reads (Tape Stop/Tape Start/Pitch Jump) —
    // IMPLEMENTATION DEVIATION FLAGGED: plan.md's Components list for this
    // phase names `juce::dsp::DelayLine` (Lagrange3rd) as the mechanism.
    // This implementation does NOT instantiate `juce::dsp::DelayLine`
    // objects for these three modes — instead, a hand-rolled 4-point,
    // 3rd-order Lagrange interpolator (`readGlitchRingBufferFractional()`,
    // the exact same all-pass-free closed-form cubic-Lagrange formula
    // `DelayLineInterpolationTypes::Lagrange3rd` implements internally)
    // reads directly from the ALREADY-CAPTURED, static `glitchRingBuffer`
    // window at an arbitrary, continuously-varying fractional index.
    // Reasoning: `juce::dsp::DelayLine`'s API models "continuously PUSH
    // live input samples, POP back after a settable delay TIME" — it has
    // no "seek/scrub to an arbitrary position within a buffer that already
    // holds captured history" operation, which is exactly what all three
    // of these modes need (their read position is a function of elapsed
    // time within the event, computed in closed form — see below — not a
    // fixed or slowly-drifting delay time). Bridging into DelayLine's push
    // model would require copying `glitchRingBuffer`'s content into a
    // second buffer every sample, wasted work and a second real-time-unsafe
    // question (sizing that copy) for zero interpolation-quality benefit
    // over reading the SAME 4-point-Lagrange formula directly. The
    // resulting interpolation ORDER (3rd-order Lagrange, 4-point) matches
    // plan.md's named component exactly; only the delivery mechanism
    // differs. `juce::dsp::WindowingFunction` (Granular Repeat) and
    // `juce::dsp::DelayLine` (unrelated: Phase 3.1/3.4's feedback loop,
    // Phase 3.3's Comb filter) both remain genuinely in use elsewhere in
    // this file — this deviation is scoped to exactly these three
    // fractional-rate Glitch modes.
    //
    // Tape Stop/Tape Start ramp math (architecture.md: "ramp playback rate
    // from 1.0->0.0 (stop) or 0.0->1.0 (start) over a musically-timed
    // ramp"): rather than accumulating a per-sample rate multiply (which
    // would need extra per-channel-call-ordering-sensitive state — see the
    // "shared vs. per-channel state" note below), the read position is
    // computed as the CLOSED-FORM INTEGRAL of a linear rate ramp over the
    // elapsed samples within the current cycle (`elapsed`, itself derived
    // from the already-existing, already-shared `glitchCurrentCycleLengthSamples`/
    // `glitchCycleSampleCounter` — no new per-sample accumulator needed):
    //   Tape Stop:  rate(t) = 1 - t/T   ->  position(t) = t - t^2/(2T)
    //   Tape Start: rate(t) = t/T       ->  position(t) = t^2/(2T)
    // (T = the event's captured window length in samples, = the full cycle
    // length for these two modes.) This is mathematically IDENTICAL to
    // accumulating `position += rate` every sample from t=0 (the discrete
    // sum converges to this integral), but is exactly reproducible/
    // stateless (a pure function of `elapsed`, independently computable by
    // each channel's call with no drift), and is C1-continuous (both
    // position AND its derivative/rate are continuous functions of `t`),
    // which is what makes the ramp start/end genuinely click-free — not
    // just "no instantaneous jump" but "no instantaneous jump in the RATE
    // OF CHANGE either" (plan.md's Phase 3.6 test criterion: "Tape
    // Stop/Start ramps sound musically correct, no clicks at ramp start/
    // end"). Note both formulas reach `T/2` at `t=T` (the ramp only ever
    // traverses half the window's nominal length over its full run, since
    // it spends the other "half" decelerating/accelerating) — a designed
    // consequence of the linear-rate assumption, not a bug.
    //
    // Pitch Jump math: an "instant playback-rate multiplier change" per
    // architecture.md — implemented as a CONSTANT-rate (non-ramping)
    // fractional read at `glitchPitchJumpRatio`, chosen ONCE per event from
    // `kGlitchPitchJumpRatios` (a fixed musical-interval set: octave/
    // fifth/fourth down and up — {1/2, 2/3, 3/4, 4/3, 3/2, 2}; UNISON
    // (1.0) IS DELIBERATELY OMITTED so this mode always produces an
    // audibly distinct "jump," per plan.md's test criterion "produces
    // correct musical intervals" — flagged here since architecture.md does
    // not pin down a specific interval set, this implementation's own
    // resolution). Position wraps modulo the window length as the ramp
    // exceeds it (`position(t) = t * ratio`, wrapped), with the SAME
    // linear loop-edge fade convention Tier-1 already uses at every wrap,
    // so a Pitch Jump event that outlives one window-length's worth of
    // elapsed time still loops click-free.
    //
    // Shared (non-per-channel) vs. per-channel state — IMPORTANT DESIGN
    // NOTE: Tier-1's existing per-channel arrays (`glitchWindowReadPos[2]`)
    // work because each channel's call independently computes an IDENTICAL
    // deterministic result (a plain increment, or — for Retrigger — a pure
    // function of already-shared state) even though it's computed twice
    // (once per channel-call per sample). That pattern is UNSAFE to reuse
    // for anything that (a) draws from `glitchRandom` at wrap time (would
    // consume/produce different values per channel, breaking stereo
    // coherence and the RNG draw-count invariant) or (b) advances a shared
    // index that must move exactly once per wrap, not twice (Slice/Random
    // Slice's slot advance; Random Repeat's re-selection). For these three
    // Tier-2 modes specifically, the wrap-triggered state change is
    // computed exactly ONCE per SAMPLE (not once per channel-call) via
    // `advanceGlitchTier2SharedState()`/`advanceGlitchGranularVoicesForThisSample()`,
    // called from processBlock()'s outer per-sample loop (mirroring the
    // existing cycle-boundary bookkeeping's own shared-once-per-sample
    // placement) — `readGlitchWindowSample()`'s per-channel calls for
    // these modes then only READ the already-resolved shared position, via
    // `glitchTier2SharedReadPos`/`glitchSliceCurrentIndex`/
    // `glitchGrainVoices`, never advance it themselves.
    //
    // RNG draw sequence invariant (carried forward from Phase 3.5, applies
    // unchanged): the two UNCONDITIONAL per-cycle-boundary draws (length
    // jitter + probability jitter/Bernoulli trial) still happen every
    // cycle boundary regardless of mode/bypass — that structural invariant
    // is untouched by this phase. Tier-2 modes that need EXTRA draws
    // (Granular Repeat's per-grain-spawn position; Random Slice's one-time
    // shuffle; Pitch Jump's one-time interval choice; Random Repeat's
    // per-wrap re-selection; Noise Burst's per-sample noise) all draw
    // strictly WITHIN that mode's own active/wet playback window — i.e.
    // only after the cycle-boundary draws for the current cycle already
    // happened, and only while that specific mode is both selected AND
    // actively playing. This does NOT violate the invariant (the
    // cycle-boundary draw COUNT/STRUCTURE per cycle stays mode-independent
    // ) — it does mean a session's overall `glitchRandom` stream position
    // (and therefore the EXACT values of later cycle-boundary draws) will
    // differ depending on which modes were active in between, which is
    // expected/unavoidable for any shared single-stream PRNG design and is
    // consistent with Phase 3.5's own reproducibility claim (identical
    // seed + identical mode/parameter automation over time -> identical
    // output; not "identical regardless of what mode happened to be
    // selected").
    //
    // Granular windowing implementation note: `juce::dsp::WindowingFunction`
    // does not expose its internal table for arbitrary per-sample random-
    // access reads (only a bulk `multiplyWithWindowingTable()` operating on
    // a caller-supplied array). So `glitchGrainWindowFn` is used exactly
    // ONCE, in `prepareToPlay()`, to bake a Hann shape into an all-ones
    // `glitchGrainWindowTable` std::vector<float> (sized once,
    // `kGlitchGrainLengthMs`-derived, never resized in processBlock) via
    // that bulk call — `processBlock()` then does plain indexed lookups
    // into the baked table, which is both correct and real-time-safe.
    //
    // Real-time safety: `glitchGrainWindowFn.fillWindowingTables()` and
    // `glitchGrainWindowTable`'s sizing/baking both happen ONLY in
    // `prepareToPlay()`, never in `processBlock()`. All Tier-2 per-sample
    // work below (grain-voice bookkeeping, closed-form position math,
    // 4-point Lagrange reads, quantization, noise draws) is bounded,
    // allocation-free, lock-free arithmetic over already-allocated state —
    // the same real-time-safety posture Phase 3.5 established.
    //=========================================================================
    static constexpr float  kGlitchMicroLoopFraction = 1.0f / 16.0f;
    static constexpr float  kGlitchBufferRepeatFraction = 0.5f;
    static constexpr int    kGlitchSliceCount = 8;
    static constexpr float  kGlitchNoiseBurstAmplitude = 0.6f;
    static constexpr float  kGlitchBitcrushBurstLevels = 7.0f; // ~3-bit fixed extreme quantization depth
    static constexpr float  kGlitchGrainLengthMs = 45.0f;
    static constexpr int    kGlitchGranularVoiceCount = 3;
    static constexpr std::array<float, 6> kGlitchPitchJumpRatios {
        0.5f, 2.0f / 3.0f, 0.75f, 4.0f / 3.0f, 1.5f, 2.0f
    };

    std::array<int, (size_t) kGlitchSliceCount> glitchSliceOrder { 0, 1, 2, 3, 4, 5, 6, 7 }; // Slice(6)/Random Slice(7) play order, (re)built at event setup
    int glitchSliceCurrentIndex = 0;    // current position within glitchSliceOrder
    int glitchTier2SharedReadPos = 0;   // shared (NOT per-channel) position-within-slice/window counter — see "Shared vs. per-channel state" doc note above
    float glitchPitchJumpRatio = 1.0f;  // musical-interval ratio chosen for Pitch Jump at event setup

    struct GlitchGrainVoice
    {
        bool active = false;
        int startRingIndex = 0; // ring-buffer index this grain reads from at phase 0
        int phase = 0;          // 0..glitchGrainLengthSamples-1
    };
    std::array<GlitchGrainVoice, (size_t) kGlitchGranularVoiceCount> glitchGrainVoices;
    int glitchGrainNextVoiceSlot = 0;     // round-robin voice-slot allocator, no dynamic voice allocation
    int glitchGrainSpawnCountdown = 0;    // samples remaining until the next grain spawn
    int glitchGrainLengthSamples = 0;     // sample-rate-derived, fixed (kGlitchGrainLengthMs), computed in prepareToPlay
    int glitchGrainSpawnIntervalSamples = 1; // glitchGrainLengthSamples / kGlitchGranularVoiceCount, computed in prepareToPlay
    juce::dsp::WindowingFunction<float> glitchGrainWindowFn { 8, juce::dsp::WindowingFunction<float>::WindowingMethod::hann }; // dummy initial size — refilled to the real fixed grain length in prepareToPlay
    std::vector<float> glitchGrainWindowTable; // baked Hann gains, glitchGrainLengthSamples long, filled once in prepareToPlay (see "Granular windowing implementation note" above)

    float readGlitchRingBufferFractional(int channel, float fractionalIndex) const;
    float readGlitchFractionalRateSample(int channel);  // Tape Stop (10) / Tape Start (11) / Pitch Jump (16)
    float readGlitchSliceSample(int channel);           // Slice (6) / Random Slice (7)
    float readGlitchRandomRepeatSample(int channel);    // Random Repeat (17)
    float readGlitchGranularSample(int channel);        // Granular Repeat (9)
    void advanceGlitchGranularVoicesForThisSample();    // called once per sample from processBlock(), Granular Repeat only
    void advanceGlitchTier2SharedState();               // called once per sample from processBlock(), Slice/Random Slice/Random Repeat only

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
