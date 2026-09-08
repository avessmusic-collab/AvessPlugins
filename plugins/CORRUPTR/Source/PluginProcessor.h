#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include "dsp/ModulationAccumulator.h"
#include "dsp/RhythmicSequencer.h"
#include "dsp/Lfo.h"
#include "dsp/ModMatrix.h"
#include "dsp/MacroEngine.h"

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

    //=========================================================================
    // Stage 3 Phase 5.6: GUI Visualization Taps (meters, sequencer playhead,
    // per-knob modulation-range indicators).
    //
    // STRICTLY ADDITIVE: every value below is a `std::memory_order_relaxed`
    // atomic store of a quantity the DSP ALREADY computes each block for its
    // own real processing (see each store site's own comment in
    // PluginProcessor.cpp for exactly which existing local/member it
    // mirrors) - no new buffers (beyond what prepareToPlay() already sizes),
    // no allocation, no locks, no change to any DSP behavior, parameter
    // semantic, or the RNG draw sequence. These getters are message-thread
    // -only (called from PluginEditor's ~30Hz juce::Timer, see
    // PluginEditor.h's Phase 5.6 doc comment) - never call them from the
    // audio thread.
    //=========================================================================
    float getVisInputPeakLevel() const noexcept  { return visInputPeakLevel.load(std::memory_order_relaxed); }
    float getVisOutputPeakLevel() const noexcept { return visOutputPeakLevel.load(std::memory_order_relaxed); }
    int   getVisSequencerStepIndex() const noexcept { return visSequencerStepIndex.load(std::memory_order_relaxed); }

    // Per-destination LIVE (post-modulation, native-unit) values for the 7
    // Mod Matrix destinations that have real, live-wired DSP (see
    // resolveModMatrixAndMacroContributions()'s own "Destinations with no
    // live DSP yet" comment for the 4 that are excluded, same scope Phase
    // 5.5 already established for its mod-indicator dots) - used by the
    // editor to draw a modulation-range indicator on each destination's
    // knob. `getVisModDriveDb()` reuses the pre-existing Phase 3.7 diagnostic
    // atomic (`phase32DriveModulationObservation`) rather than duplicating
    // it under a new name.
    float getVisModDriveDb() const noexcept              { return phase32DriveModulationObservation.load(std::memory_order_relaxed); }
    float getVisModFoldPct() const noexcept               { return visModFoldPct.load(std::memory_order_relaxed); }
    float getVisModBitDepth() const noexcept              { return visModBitDepth.load(std::memory_order_relaxed); }
    float getVisModFilterCutoffHz() const noexcept        { return visModFilterCutoffHz.load(std::memory_order_relaxed); }
    float getVisModMixPct() const noexcept                { return visModMixPct.load(std::memory_order_relaxed); }
    float getVisModFeedbackAmountPct() const noexcept     { return visModFeedbackAmountPct.load(std::memory_order_relaxed); }
    float getVisModGlitchProbabilityPct() const noexcept  { return visModGlitchProbabilityPct.load(std::memory_order_relaxed); }

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
    static constexpr float kFeedbackMaxSafeGain = 0.20f;      // user request 2026-09-07: feedback A LOT weaker at 100% (was 0.85->0.40->0.20). tanh(1)*0.20 ~= 0.15 per-pass loop gain
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
    // Stage 2 Phase 3.2: Unified Modulation Accumulator (Isolated) -- UPDATED
    // IN PHASE 3.7, see that section's doc comment below for the full story.
    //
    // See dsp/ModulationAccumulator.h for the generic, reusable combine-math
    // class (Performance Trigger hard-override, else additive Sequencer +
    // Mod Matrix + Macro combine, always clamped to the destination's valid
    // range - architecture.md's recommended default rule). This section
    // wires that class up to ONE destination only (`drive`, 0-40dB) per
    // plan.md's recommended build order, originally (Phase 3.2) using
    // SYNTHETIC stand-ins for the Sequencer and Mod Matrix contributions and
    // the REAL `macroDamage` APVTS parameter for the Macro contribution.
    //
    // AS OF PHASE 3.7: the Sequencer stand-in has been RETIRED and replaced
    // with the REAL Rhythmic Sequencer's `drive`-lane contribution (see the
    // Phase 3.7 section below, `sequencerDriveContributionDb`) -- the
    // `phase32SyntheticSequencerPhase` member this comment used to describe
    // no longer exists.
    //
    // AS OF PHASE 3.8: the Mod Matrix stand-in is ALSO now RETIRED --
    // `phase32SyntheticModMatrixPhase` (a free-running synthetic-sine phase
    // this comment used to describe) no longer exists, replaced by the REAL
    // 8-slot Mod Matrix's resolved `drive`-destination total (see the Phase
    // 3.8 section below, `modMatrixDestinationTotals`). The Macro
    // contribution is likewise no longer a placeholder inline curve -- see
    // the Phase 3.8 section's `macroContributions` (dsp/MacroEngine.h).
    // `phase32DriveModulationObservation` is likewise no longer "diagnostic
    // only" -- as of Phase 3.7 it is CONSUMED by the Distortion Engine's real
    // drive-gain smoothing target (see processBlock()'s Phase 3.3 per-block
    // parameter-read section) -- this is the first destination where the
    // unified modulation accumulator's output reaches live DSP.
    //=========================================================================
    std::atomic<bool> modulationAccumulatorSelfTestPassed { false }; // set once in the constructor (see .cpp)
    std::atomic<float> phase32DriveModulationObservation { 0.0f };   // AS OF PHASE 3.7: consumed by driveGainSmoothed's target (no longer diagnostic-only) -- last computed modulated `drive` value

    //=========================================================================
    // Stage 2 Phase 3.7: Rhythmic Sequencer -- architecture.md component #5,
    // plan.md's "Phase 3.7: Rhythmic Sequencer".
    //
    // `rhythmicSequencer` (see dsp/RhythmicSequencer.h for the full class
    // doc comment) owns the 10-lane x up-to-32-step pattern data as custom,
    // non-APVTS state (locked Stage 0 decision) plus the lock-free
    // double-buffered audio-thread snapshot mechanism architecture.md's
    // Thread Boundaries section specifies. `getRhythmicSequencer()` below
    // exposes it publicly (message-thread only) so a future GUI can call
    // its pattern-editing/pattern-operation methods directly, the same
    // convention `getAPVTS()` already establishes for APVTS access.
    //
    // Host sync: uses the SAME JUCE 8 `AudioPlayHead::getPosition()`
    // `std::optional<PositionInfo>` API Phase 3.5 already established for
    // `glitchHostBpm` (getIsPlaying() + getPpqPosition() instead of
    // getBpm()). This method runs BEFORE Phase 3.5's own per-block BPM
    // read (see the call site in processBlock()), so it issues its OWN
    // `getPosition()` call rather than sharing Phase 3.5's -- both queries
    // are cheap, allocation-free, and real-time-safe (a plain stack-based
    // std::optional read), so calling getPosition() twice per block has no
    // meaningful cost; this is a second, independent use of the SAME
    // established API/pattern, not a second DIFFERENT playhead-query
    // mechanism.
    //
    // NO-TRANSPORT FALLBACK (architecture.md, explicit MVP requirement):
    // if the host provides no AudioPlayHead, no PositionInfo, isPlaying()
    // is false, or the host doesn't report ppqPosition at all, the
    // sequencer FREEZES AT STEP 0 -- an explicit, deliberate branch in
    // updateSequencerStepAndContributions() (see PluginProcessor.cpp),
    // not an accidental fallthrough. (architecture.md phrases this as
    // "freeze at step 0 / hold last value" -- freeze-at-step-0 was chosen
    // as the simpler, unambiguous, easily user-verifiable of the two
    // options: Standalone with no transport running always reads step 0's
    // values, never an indeterminate "whatever step it happened to be on
    // last.")
    //
    // PER-BLOCK (not per-sample) step-boundary resolution -- FLAGGED DESIGN
    // RESOLUTION: architecture.md does not specify per-sample vs. per-block
    // granularity for step advancement. This implementation resolves it
    // once per BLOCK (unlike Phase 3.5's Glitch Engine, whose cycle counter
    // advances per-sample from a per-block BPM read) -- full reasoning is
    // documented at updateSequencerStepAndContributions()'s definition in
    // PluginProcessor.cpp.
    //
    // Destination wiring -- "Sequencer output enters parameter modulation
    // ONLY through ModulationAccumulator::accumulate()" (task requirement):
    // every one of the 10 lanes' step values is converted to a
    // destination-units contribution scalar here (the
    // `sequencerXContributionY` members below), then combined via
    // ModulationAccumulator::accumulate() AT each destination's own
    // existing per-block parameter-read call site (drive: this section's
    // Phase 3.2 block above; mix/filterCutoff/bitDepth/sampleRateReduction:
    // Phase 3.3's per-block section; glitchProbability: Phase 3.5's
    // per-block section; volume/gate: this section's own new combined-gain
    // application point in the per-sample loop) -- see each call site in
    // PluginProcessor.cpp for its own per-lane comment. Pan and Pitch have
    // NO live destination yet (Stereo and Pitch/Frequency FX are explicitly
    // post-MVP per architecture.md's Scope Reconciliation Note) -- their
    // contributions are still computed and clamped via the identical
    // accumulate() path, stored in diagnostic atomics
    // (`sequencerPanObservation`/`sequencerPitchObservation`) for a future
    // Stage, matching Phase 3.2's own "computed but not yet applied to any
    // DSP" precedent exactly.
    //
    // Per-lane modulation-depth scaling (FLAGGED DESIGN RESOLUTION --
    // architecture.md does not specify depth per lane): lanes with a
    // pre-existing base APVTS knob (Drive/Mix/Filter Cutoff/Bit Depth/
    // Sample Rate Reduction/Glitch Probability) scale by
    // `kSequencerModDepthFraction` (0.5) of the destination's full range,
    // so the sequencer can swing the accumulated result up to +/-50% of the
    // range around the user's own base setting without ever fully
    // overriding it. Volume/Gate (new, sequencer-owned destinations with no
    // competing base setting) use their full range instead. See
    // PluginProcessor.cpp's updateSequencerStepAndContributions() for the
    // exact per-lane formulas.
    //
    // Volume + Gate consumption: combined into ONE linear gain scalar
    // (`sequencerVolumeGateGainTarget`), smoothed via
    // `sequencerVolumeGateGainSmoothed` (8ms ramp -- fast enough to feel
    // rhythmically tight/gate-like, still click-free), applied once per
    // sample in the merged per-sample loop immediately after Master Mix's
    // dry/wet combine and before that loop's existing final isfinite()
    // guard (so a corrupt combined gain is still caught by that guard).
    //
    // Pattern-operation RNG: `rhythmicSequencer`'s internal `editRandom`
    // (message-thread-only, see dsp/RhythmicSequencer.h) is used for
    // Random/Mutate/Syncopate/Humanize -- NEVER the audio-thread
    // `glitchRandom` instance (Phase 3.5's determinism invariant for
    // `glitchRandom`'s own draw sequence is preserved unchanged).
    //
    // Real-time safety: `rhythmicSequencer.getActiveSnapshot()` is a single
    // relaxed atomic load (no allocation, no locks). All ten per-block
    // sequencer contribution scalars below are plain float arithmetic,
    // bounded, allocation-free. `sequencerVolumeGateGainSmoothed` is reset/
    // sized only in prepareToPlay(), same convention as every other
    // SmoothedValue in this file.
    //=========================================================================
    RhythmicSequencer rhythmicSequencer; // pattern data + double-buffered audio-thread snapshot (dsp/RhythmicSequencer.h)

public:
    // Message-thread only (pattern editing/pattern operations from a future
    // GUI) -- same accessor convention as getAPVTS() above.
    RhythmicSequencer& getRhythmicSequencer() noexcept { return rhythmicSequencer; }

private:
    bool sequencerEnabledFlag = true;
    int sequencerRateIndex = 2;   // sequencerRate choice index (default "1/16")
    int sequencerNumSteps = 16;   // sequencerSteps choice resolved to an actual count (16 or 32)
    int sequencerLastStepIndex = -1; // diagnostic/inspection only -- last resolved step index (or 0 during the no-transport freeze)

    // Per-lane raw contribution scalars, already converted to the
    // destination's native units (e.g. dB for Drive, Hz for Filter Cutoff),
    // computed once per block by updateSequencerStepAndContributions() and
    // consumed at each destination's own existing per-block parameter-read
    // call site (see the doc comment above for the full list).
    float sequencerDriveContributionDb              = 0.0f;
    float sequencerMixContributionPct               = 0.0f;
    float sequencerFilterCutoffContributionHz       = 0.0f;
    float sequencerBitDepthContributionBits         = 0.0f;
    float sequencerSampleRateContributionFactor     = 0.0f;
    float sequencerGlitchProbabilityContributionPct = 0.0f;

    float sequencerVolumeGateGainTarget = 1.0f; // combined Volume+Gate linear gain (see doc comment above)
    juce::SmoothedValue<float> sequencerVolumeGateGainSmoothed;

    // Pan/Pitch: NOT YET CONSUMED (see doc comment above) -- diagnostic
    // atomics only, same convention as Phase 3.2's
    // phase32DriveModulationObservation was before Phase 3.7 wired it live.
    std::atomic<float> sequencerPanObservation { 0.0f };
    std::atomic<float> sequencerPitchObservation { 0.0f };

    static constexpr float kSequencerModDepthFraction = 0.5f; // see doc comment above
    static constexpr float kSequencerVolumeRangeDb = 24.0f;   // Volume lane's full +/-dB swing (new destination, no competing base -- see doc comment above)

    void updateSequencerStepAndContributions(); // called once per block from processBlock(), before the Phase 3.2/3.7 drive-accumulate block

    //=========================================================================
    // Stage 2 Phase 3.8: Modulation Matrix (8 slots) + 4 LFOs + Macro System
    // (8 macros) -- architecture.md components #8 ("Modulation Matrix + 4
    // LFOs") and #9 ("Macro System"), plan.md's "Phase 3.8: Modulation
    // Matrix + 4 LFOs + Macros".
    //
    // Three new header-only classes (dsp/Lfo.h, dsp/ModMatrix.h,
    // dsp/MacroEngine.h) implement the actual DSP/routing logic, following
    // this file's established pattern (dsp/ModulationAccumulator.h,
    // dsp/RhythmicSequencer.h) of keeping generic/reusable logic out of
    // PluginProcessor itself. See each header's own top doc comment for its
    // full design rationale; this comment covers the INTEGRATION into
    // PluginProcessor specifically.
    //
    // CALL ORDER (processBlock()): `resolveModMatrixAndMacroContributions()`
    // runs BEFORE `updateSequencerStepAndContributions()` (both near the top
    // of processBlock(), after the dry-signal capture and this phase's new
    // MIDI-CC scan) -- the REVERSE of a naive "Sequencer feeds Mod Matrix's
    // Sequencer source" ordering. This is deliberate: the Mod Matrix's
    // "Sequencer" source (see below) and the Envelope/Audio Level sources
    // are all documented as ONE-BLOCK-OLD readings (this block's resolution
    // uses values computed during the PREVIOUS block), which removes any
    // same-block ordering dependency and lets
    // `updateSequencerStepAndContributions()`'s own Gate-lane accumulate()
    // call (see its Phase 3.7 doc comment, now UPDATED to consume this
    // phase's real `macroContributions.gateGainOffset`) run AFTER this
    // method with `macroContributions` already populated for the current
    // block.
    //
    // MODULATION SOURCES (10, matching parameter-spec.md's modSlotNSource
    // choice order exactly -- see ModMatrix::Source):
    //   - LFO 1-4: `lfos[0..3]`, each a dsp/Lfo.h instance, resolved at
    //     CONTROL RATE (once per block -- see Lfo.h's own doc comment for
    //     the full flagged-resolution rationale). Rate is either the raw
    //     lfoNRate parameter (Hz, free-running) or, when lfoNSync is true,
    //     that SAME parameter value re-mapped to a tempo-synced Hz via
    //     `Lfo::mapToSyncedHz()` against a THIRD independent
    //     `AudioPlayHead::getPosition()`/`getBpm()` read this block (the
    //     same cheap, real-time-safe, allocation-free per-block query
    //     pattern Phase 3.5's `glitchHostBpm` and Phase 3.7's
    //     `updateSequencerStepAndContributions()` already each
    //     independently establish -- not a new/different mechanism).
    //   - Envelope / Audio Level: two `juce::dsp::BallisticsFilter<float>`
    //     instances (`envelopeFollowerBallistics` -- musical ~10ms
    //     attack/~150ms release; `audioLevelBallistics` -- fast ~1ms
    //     attack/~30ms release, near-instantaneous level, the concrete
    //     difference between these two named sources), BOTH tapping the
    //     SAME mono-summed signal, tracked at AUDIO RATE (once per SAMPLE,
    //     inside the merged per-sample loop -- BallisticsFilter is designed
    //     for per-sample ballistics, unlike this class's own control-rate
    //     LFOs) -- see the per-sample tap site in processBlock() for the
    //     TAP-POINT resolution: FLAGGED, architecture.md does not specify a
    //     tap point, post-Input-Gain (the conventional choice for an
    //     envelope-follower mod source) is used here. Each block's Mod
    //     Matrix resolution uses the LAST sample's tracked value from the
    //     PREVIOUS block (`envelopeFollowerLastValue`/`audioLevelLastValue`)
    //     -- a standard one-block-old envelope tap, no audible consequence
    //     at typical block sizes, and avoids any same-block
    //     ordering dependency (see "CALL ORDER" above).
    //   - Sequencer: FLAGGED DESIGN RESOLUTION -- this is the Mod Matrix's
    //     OWN generic tap into the Rhythmic Sequencer's pattern data,
    //     SEPARATE from the Sequencer's 10 pre-wired fixed lanes (Phase
    //     3.7). architecture.md does not specify which (if any specific)
    //     lane should feed this generic source choice -- this
    //     implementation reuses the Drive lane's raw value (bipolar -1..1)
    //     as a representative general-purpose reading, at the PREVIOUS
    //     block's resolved step index (`sequencerLastStepIndex`, one-block-
    //     old for the same reason as Envelope/Audio Level above).
    //   - Random: a fresh draw every block from `modMatrixRandom` (see
    //     below).
    //   - MIDI CC: `midiCcSourceValue`, updated by a small MIDI-parsing
    //     loop near the top of processBlock() (this phase's first actual
    //     use of the `midiMessages` parameter -- previously
    //     `juce::ignoreUnused()`d). FLAGGED: no per-slot "which CC number"
    //     parameter exists in the locked parameter-spec.md (modSlot fields
    //     are only Source/Destination/Amount/Enable) -- a single fixed CC
    //     number (`kModMatrixMidiCcNumber`, CC1/mod wheel) is used for
    //     every slot that selects "MIDI CC," per architecture.md's "any
    //     incoming CC value (0-127 -> 0.0-1.0) usable as a modulation
    //     source." The held value persists across blocks with no new
    //     messages (plain audio-thread-only member -- MIDI is already
    //     parsed on the audio thread per architecture.md's MIDI Routing
    //     section, no cross-thread concern).
    //   - Macro: FLAGGED DESIGN RESOLUTION -- same "no per-slot
    //     sub-selector parameter exists" gap as MIDI CC above (no per-slot
    //     "which of the 8 macros" parameter exists either) -- the AVERAGE
    //     of all 8 macros' 0-100% values, normalized to 0-1, is used as the
    //     generic "Macro" Mod Matrix source.
    //
    // DEDICATED RNG (`modMatrixRandom`, DETERMINISM task requirement): used
    // for LFO S&H/Random/Smooth-Random/Random-Walk shape draws (passed by
    // reference into each `Lfo::advanceAndGetValue()` call) AND the Mod
    // Matrix's own "Random" source draw. Deliberately a SEPARATE
    // `juce::Random` instance from BOTH the audio-thread `glitchRandom`
    // (Phase 3.5) and `RhythmicSequencer`'s message-thread `editRandom`
    // (Phase 3.7) -- structurally guarantees this class's randomness can
    // never perturb either of those draw sequences, since no code path ever
    // shares the instance. UNLIKE `glitchRandom`, this RNG's seed is NOT
    // persisted in getStateInformation()/setStateInformation() and is NOT
    // deliberately reseeded from `juce::Random::getSystemRandom()` the way
    // `glitchRandom`'s initial seed is -- it simply uses `juce::Random`'s
    // own default-constructor seeding. FLAGGED RESOLUTION: architecture.md's
    // explicit "seed stored in custom state... presets reproduce identical
    // behavior across sessions" reproducibility requirement is scoped
    // specifically to component #4 (Glitch Engine's chaos/randomization
    // text) -- neither component #8 (Mod Matrix/LFOs) nor component #9
    // (Macros) makes the same reproducibility claim, so this implementation
    // does not extend it here; only the "must not perturb glitchRandom's
    // draw sequence" requirement (which is explicit and unconditional) is
    // honored, and it is honored structurally (separate instance, never
    // shared) rather than via seed persistence.
    //
    // MOD MATRIX RESOLUTION (`ModMatrix::resolve()`, dsp/ModMatrix.h): all
    // 8 slots' current Source/Destination/Amount/Enable state is read from
    // the CACHED raw parameter pointers below (see "CACHED PARAMETER
    // POINTERS"), producing `modMatrixDestinationTotals` -- one raw
    // (roughly -1..+1-per-contributing-slot, unbounded-sum-across-slots)
    // total PER of the 12 possible destinations (matching
    // parameter-spec.md's modSlotNDestination choice order exactly -- see
    // ModMatrix::Destination). `modMatrixEnabled=false` (module-off,
    // ENABLE-semantic polarity matching `sequencerEnabled`/
    // `modMatrixEnabled`'s documented convention) zeroes every total for
    // this block rather than calling `ModMatrix::resolve()` at all.
    //
    // DESTINATION WIRING -- "Mod Matrix/Macro output enters parameter
    // modulation ONLY through ModulationAccumulator::accumulate()" (task
    // requirement, mirroring Phase 3.7's identical requirement for the
    // Sequencer): each of `modMatrixDestinationTotals`' 12 raw totals is
    // converted to destination-units at THAT destination's own existing (or,
    // for Fold/Feedback Amount, NEWLY added -- see below) per-block
    // accumulate() call site, reusing the SAME `kModMatrixDepthFraction`
    // (0.5, deliberately the SAME numeric value as
    // `kSequencerModDepthFraction`) half-of-full-range convention
    // RhythmicSequencer's lanes already established, so the Mod Matrix can
    // swing a destination up to +/-50% of its full range around the
    // combined base+Sequencer+Macro value without ever fully overriding it:
    //   - Drive, Mix, Filter Cutoff, Bit Depth, Sample Rate Reduction,
    //     Glitch Probability: the 6 PRE-EXISTING call sites Phase 3.7
    //     stubbed at `modMatrixContribution=0.0f` -- filled in with real
    //     values this phase (Filter Cutoff has no macro target per
    //     architecture.md's routing table, so its macro argument stays a
    //     literal `0.0f` with an explanatory comment, not a stub).
    //   - Fold, Feedback Amount: TWO NEW call sites (architecture.md's Mod
    //     Matrix destination list and macroDamage's routing table both name
    //     these as targets, but neither `fold` nor `feedbackAmount` had ANY
    //     accumulate() call before this phase -- Phase 3.3/3.4 read them as
    //     raw APVTS values directly). SAFETY NOTE for Feedback Amount:
    //     wiring it through the accumulator does NOT weaken Phase 3.1's
    //     safety guarantee -- `ModulationAccumulator::accumulate()` clamps
    //     its result to [0,100] BEFORE that value ever reaches the existing
    //     `tanh()` soft-clamp formula, so the soft-clamp still only ever
    //     sees a valid 0-100% input, exactly as before; this adds an EXTRA
    //     clamp layer upstream of the existing one, it does not remove or
    //     bypass it.
    //   - Gate (the Sequencer's own, Phase-3.7-introduced destination, in
    //     `updateSequencerStepAndContributions()`): the PRE-EXISTING
    //     `macroContribution=0.0f` stub is filled with
    //     `macroContributions.gateGainOffset` (macroRhythm's "Sequencer/
    //     Glitch gate intensity" routing) -- its `modMatrixContribution`
    //     stays 0.0f (no "Gate" entry exists in the Mod Matrix's 12-item
    //     destination list).
    //   - Volume (the Sequencer's other Phase-3.7 destination): BOTH stubs
    //     stay literal `0.0f` -- no "Volume" Mod Matrix destination exists,
    //     and no macro's documented routing table targets it specifically.
    //   - Glitch Size, Pitch, Pan, Width: the remaining 4 of 12 Mod Matrix
    //     destinations (plus macroWidth's own contribution, for Width
    //     specifically) have NO live DSP destination yet (Stereo and
    //     Pitch/Frequency FX are explicitly post-MVP per architecture.md's
    //     Scope Reconciliation Note; "Glitch Size" has no continuously-
    //     modulatable APVTS parameter -- `glitchBufferLength` is a discrete
    //     Choice). Computed and clamped through the identical
    //     ModulationAccumulator path anyway, stored in diagnostic atomics
    //     below, matching RhythmicSequencer's `sequencerPanObservation`/
    //     `sequencerPitchObservation` precedent exactly (this file's
    //     established pattern for a destination whose downstream engine
    //     doesn't exist yet).
    //
    // Real-time safety: all 53 cached raw parameter pointers below are
    // fetched exactly ONCE (constructor, see .cpp), never re-looked-up by
    // string per block. `envelopeFollowerBallistics`/`audioLevelBallistics`
    // are prepared only in prepareToPlay(). `modMatrixRandom.nextFloat()`,
    // `Lfo::advanceAndGetValue()`, `ModMatrix::resolve()`, and
    // `MacroEngine::resolve()` are all bounded, allocation-free, lock-free
    // arithmetic over already-allocated/already-cached state. The MIDI scan
    // is a plain bounded loop over `midiMessages` (already guaranteed
    // real-time-safe/allocation-free by JUCE's MidiBuffer iterator design).
    //=========================================================================
    static constexpr int kModMatrixMidiCcNumber = 1;      // FLAGGED default CC (mod wheel) -- see doc comment above
    static constexpr float kModMatrixDepthFraction = 0.5f; // reuses kSequencerModDepthFraction's established half-range-swing convention (see doc comment above)

    std::array<Lfo, 4> lfos;
    juce::Random modMatrixRandom; // dedicated RNG -- see "DEDICATED RNG" doc comment above
    std::array<double, 4> lfoLastValue { 0.0, 0.0, 0.0, 0.0 }; // diagnostic/inspection convenience only -- not required for correctness, ModMatrix reads advanceAndGetValue()'s return value directly each block

    juce::dsp::BallisticsFilter<float> envelopeFollowerBallistics; // "Envelope" Mod Matrix source (slower, musical ballistics)
    juce::dsp::BallisticsFilter<float> audioLevelBallistics;       // "Audio Level" Mod Matrix source (faster, near-instantaneous ballistics -- the concrete difference from Envelope above)
    float envelopeFollowerLastValue = 0.0f;
    float audioLevelLastValue = 0.0f;

    float midiCcSourceValue = 0.0f; // last-received normalized (0-1) value for kModMatrixMidiCcNumber, held across blocks -- see "MODULATION SOURCES" doc comment above

    //=========================================================================
    // CACHED PARAMETER POINTERS -- fetched ONCE in the constructor (see
    // .cpp), a deliberate, explicitly-directed exception to this file's
    // OTHERWISE-established convention of re-fetching every parameter fresh
    // every block via `parameters.getRawParameterValue(...)` (see e.g.
    // processBlock()'s large Phase 3.3 per-block parameter-read section).
    // 53 string-keyed lookups (32 mod-slot + 12 LFO + 8 macro + 1
    // modMatrixEnabled) every single block would be needless repeated
    // hashing for parameters this phase's own per-block work already reads
    // in bulk; caching the `std::atomic<float>*` once (constructed AFTER
    // `parameters` in the member-init-list, so these pointers are already
    // valid by the time the constructor body runs) and re-`load()`-ing them
    // every block is standard, real-time-safe APVTS practice.
    //=========================================================================
    std::atomic<float>* modMatrixEnabledParam = nullptr;
    std::array<std::atomic<float>*, 4> lfoRateParam {};
    std::array<std::atomic<float>*, 4> lfoShapeParam {};
    std::array<std::atomic<float>*, 4> lfoSyncParam {};
    std::array<std::atomic<float>*, 8> modSlotSourceParam {};
    std::array<std::atomic<float>*, 8> modSlotDestinationParam {};
    std::array<std::atomic<float>*, 8> modSlotAmountParam {};
    std::array<std::atomic<float>*, 8> modSlotEnableParam {};
    std::atomic<float>* macroDamageParamCached = nullptr;
    std::atomic<float>* macroCrushParamCached = nullptr;
    std::atomic<float>* macroGlitchParamCached = nullptr;
    std::atomic<float>* macroChaosParamCached = nullptr;
    std::atomic<float>* macroRhythmParamCached = nullptr;
    std::atomic<float>* macroMovementParamCached = nullptr;
    std::atomic<float>* macroWidthParamCached = nullptr;
    std::atomic<float>* macroMixParamCached = nullptr;

    // Per-block resolved outputs, consumed at each destination's own
    // per-block accumulate() call site -- see "DESTINATION WIRING" doc
    // comment above.
    std::array<float, ModMatrix::kNumDestinations> modMatrixDestinationTotals {}; // raw units, NOT yet scaled to any destination's physical range -- see dsp/ModMatrix.h
    MacroEngine::Contributions macroContributions;                               // physical-units-ready -- see dsp/MacroEngine.h

    // Destinations with no live DSP yet -- see "DESTINATION WIRING" doc
    // comment above (Glitch Size / Pitch / Pan / Width).
    std::atomic<float> modMatrixGlitchSizeObservation { 0.0f };
    std::atomic<float> modMatrixPanObservation { 0.0f };
    std::atomic<float> modMatrixPitchObservation { 0.0f };
    std::atomic<float> modMatrixWidthObservation { 0.0f }; // combines the Mod Matrix's own "Width" destination total + macroWidth's contribution (both target the same not-yet-existing Stereo-width parameter)

    void resolveModMatrixAndMacroContributions(double blockDurationSeconds, int numSamples); // called once per block from processBlock(), BEFORE updateSequencerStepAndContributions() -- see "CALL ORDER" doc comment above. `numSamples` added in Phase 3.9 so this method can tick the XY Pad SmoothedValues via skip() at the correct base-rate pace before folding their contribution into macroDamage/macroGlitch -- see "Phase 3.9" doc comment below.

    //=========================================================================
    // Stage 2 Phase 3.9 (FINAL DSP phase): XY Pad, Performance Mode Triggers,
    // Oversampling/Quality Engine -- architecture.md components #10 ("XY Pad
    // with Inertia"), #11 ("Performance Mode Triggers"), #12
    // ("Oversampling/Quality Engine"), plan.md's "Phase 3.9: XY Pad,
    // Performance Triggers, Oversampling/Quality Modes".
    //
    // ---- XY PAD (component #10) ----
    // `xyPadX`/`xyPadY`/`xyPadSmoothing` are ALREADY declared APVTS
    // parameters (Stage 1). This phase adds the actual glide mechanism:
    // two `juce::SmoothedValue<float>` (0-100, matching the parameters'
    // own %-range), target set from the raw APVTS value each block, ramp
    // LENGTH controlled by `xyPadSmoothing` (0-500ms). Per architecture.md:
    // "C++ side calls SmoothedValue::setTargetValue() and lets the built-in
    // ramp glide toward it every block -- the standard, correct JUCE
    // mechanism." Ticked via `skip(numSamples)` (NOT `getNextValue()` in a
    // per-sample loop) because the two consumers of this value
    // (`resolveModMatrixAndMacroContributions()`'s macroDamage/macroGlitch
    // blend) run at CONTROL RATE, once per block -- `skip()` is the
    // JUCE-idiomatic way to advance a per-sample-model SmoothedValue by N
    // samples at once and read its resulting value, without generating (or
    // needing) per-sample output, exactly analogous to how Lfo.h's
    // control-rate consumers already work (see that file's own "CONTROL-
    // RATE RESOLUTION" doc comment) -- but unlike Lfo.h's OWN internal
    // phase accumulator (which is deliberately NOT sample-rate-based),
    // `juce::SmoothedValue` genuinely IS a per-sample model, so `skip()` is
    // the correct API for this specific case, not a workaround.
    //
    // RAMP-LENGTH CHANGE HANDLING (FLAGGED DESIGN RESOLUTION):
    // `xyPadSmoothing` is itself a live-automatable parameter, but
    // `juce::SmoothedValue::reset(sampleRate, newRampSeconds)` -- the ONLY
    // API to change a SmoothedValue's ramp length -- also SNAPS
    // current-value-to-target (via its internal `setCurrentAndTargetValue`
    // call), which would defeat the whole "glide, not snap" purpose if
    // called every block. This implementation change-detects
    // `xyPadSmoothing` (matching this file's established
    // `lastFilterTypeIndex`/`lastGlitchModeIndex`-style per-block
    // change-detection convention) and, ONLY on an actual change, captures
    // the CURRENT (mid-glide) value, calls `reset()` with the new ramp
    // length, then immediately restores the captured current value via
    // `setCurrentAndTargetValue()` before re-applying the real target --
    // this preserves the in-flight glide position (no snap-to-target
    // artifact) while still picking up the new ramp length for FUTURE
    // target changes. `reset()` itself is allocation-free (pure arithmetic
    // -- recomputes an internal ramp-length-in-samples value), so calling
    // it occasionally from the audio thread (only on an actual parameter
    // change, not every block) is real-time-safe.
    //
    // DEFAULT MAPPING (FLAGGED DESIGN RESOLUTION -- deviates from
    // architecture.md's own RECOMMENDATION, not a locked requirement):
    // architecture.md component #10 recommends "a small internal routing
    // choice param (xyPadXTarget, xyPadYTarget)" so either axis can drive
    // any macro/parameter. NO such parameter exists in the LOCKED
    // parameter-spec.md (v2, immutable per its own "CRITICAL CONTRACT" --
    // only xyPadX/xyPadY/xyPadSmoothing are declared, no XTarget/YTarget
    // choices). Since the parameter spec is the authoritative, immutable
    // contract and architecture.md's routing-param suggestion was only ever
    // a "recommend," not a requirement it locked in, this implementation
    // hardcodes architecture's own stated DEFAULT mapping instead: X ->
    // macroDamage, Y -> macroGlitch (architecture.md component #10:
    // "Default mapping: X->macroDamage, Y->macroGlitch"). A future mockup
    // iteration adding XTarget/YTarget choice parameters could make this
    // user-assignable without changing this phase's underlying glide
    // mechanism.
    //
    // CONTRIBUTION DEPTH (FLAGGED -- architecture.md does not specify how
    // strongly the pad should drive its target macro): reuses this file's
    // established `kSequencerModDepthFraction`/`kModMatrixDepthFraction`
    // half-of-full-range-swing convention (`kXyPadModDepthFraction`, same
    // 0.5 value) so the pad can swing its target macro up to +/-50% of the
    // macro's own 0-100% range around whatever the macro's OWN knob is
    // currently set to, additively, via
    // `resolveModMatrixAndMacroContributions()`'s macroDamagePct/
    // macroGlitchPct computation (clamped 0-100 before MacroEngine::resolve()
    // runs) -- never fully overriding the base macro knob, same
    // non-destructive-blend philosophy as every other additive modulation
    // source in this file.
    //
    // ---- PERFORMANCE MODE TRIGGERS (component #11, 7 triggers) ----
    // All 7 `performanceKill`..`performanceChaos` `AudioParameterBool`s are
    // ALREADY declared (Stage 1). Each trigger's boolean state is resolved
    // fresh every block (matching this file's default per-block
    // `getRawParameterValue()` convention -- not cached, since there are
    // only 7 and they are not read in a hot per-sample loop) into LOCAL
    // `bool` variables near the top of `processBlock()`, then threaded
    // through to every accumulate() call site each trigger affects (drive
    // was already wired in Phase 3.2 as the pattern's first exercise; this
    // phase extends the SAME override-wins/releases-cleanly pattern to the
    // other 6). "Preset diff" VALUES below are this implementation's own
    // resolution (architecture.md names the CONCEPT -- "engages a
    // predefined preset diff... for instant dramatic change" -- without
    // pinning down concrete per-trigger numeric deltas), documented in full
    // at each call site in `processBlock()`:
    //   - performanceKill: hard-overrides the Gate lane's EXISTING
    //     `ModulationAccumulator::accumulate()` call (Phase 3.7's
    //     `updateSequencerStepAndContributions()`) to 0.0 (fully closed,
    //     the Gate lane's own native 0-1 range) -- a genuine, guaranteed
    //     full mute applied as the LAST multiplicative gain stage before
    //     Master Mix's post-mix `isfinite()` guard, independent of
    //     `outputGain`'s narrower -24..+24dB range (which could not reach
    //     genuine silence).
    //   - performanceGlitch: overrides `glitchProbability`'s existing
    //     accumulate() call to 100% (guarantees a glitch event fires) and
    //     shares `chaos`'s override (60%, see performanceChaos below).
    //   - performanceDestroy: `drive`'s override (already wired, Phase
    //     3.2/3.7/3.8, 40dB/max) is joined by NEW overrides on `fold`
    //     (100%), `bitDepth` (1 bit, minimum), `sampleRateReduction` (48x,
    //     maximum), and `feedbackAmount` (90%) -- all via their EXISTING
    //     accumulate() call sites. SAFETY NOTE: `feedbackAmount`'s override
    //     still passes through the tanh soft-clamp (`kFeedbackMaxSafeGain`)
    //     downstream exactly as any other value would -- Performance
    //     Triggers are a HIGH-PRIORITY override within the modulation
    //     accumulator layer, not a bypass of Phase 3.1's independent safety
    //     layer beneath it.
    //   - performanceFreeze / performanceReverse / performanceStutter: these
    //     three target the DISCRETE `glitchMode`/`glitchBufferLength`
    //     Choice parameters, which `ModulationAccumulator` cannot express
    //     (it only combines CONTINUOUS float ranges) -- FLAGGED DEVIATION:
    //     implemented as a direct, highest-priority override of the
    //     already-block-resolved `glitchModeIndex`/`glitchBufferLengthIndex`
    //     LOCAL variables (see the glitch per-block section in
    //     processBlock()), the natural discrete-parameter equivalent of the
    //     accumulator's override-wins-then-releases-instantly semantic
    //     (re-evaluated fresh from the boolean flag every block, never
    //     latched). Name-to-mode pairing is intuitive by design
    //     (performanceFreeze forces glitchMode=Freeze, performanceReverse
    //     forces glitchMode=Reverse, performanceStutter forces
    //     glitchMode=Stutter) -- matching architecture.md's OWN explicit
    //     worked example ("performanceFreeze forces glitchMode=Freeze +
    //     near-zero glitch buffer length"). performanceFreeze ALSO forces
    //     `glitchBufferLengthIndex=0` ("near-zero," per that same worked
    //     example). All three ALSO force `glitchProbability` to 100%
    //     (joining performanceGlitch/performanceChaos in that shared OR'd
    //     override condition below) -- otherwise a user with
    //     `glitchProbability=0%` (the parameter's own default) would see
    //     NO audible effect from forcing just the mode/buffer-length, which
    //     would contradict "instant dramatic change." MULTI-TRIGGER
    //     PRECEDENCE (FLAGGED, architecture.md does not specify): if more
    //     than one of these three is simultaneously active (e.g. two MIDI
    //     notes held at once), first-match-wins in the fixed order
    //     Freeze > Reverse > Stutter.
    //   - performanceChaos: overrides `chaos`'s existing accumulate() call
    //     to 100% (maximum unpredictability) and shares `glitchProbability`'s
    //     100% override (joining the OR'd condition above). If
    //     performanceGlitch is ALSO active simultaneously, performanceChaos's
    //     100% chaos value takes precedence over performanceGlitch's own
    //     60% (higher-intensity trigger wins on a shared destination,
    //     FLAGGED, architecture.md does not specify multi-trigger
    //     precedence for a shared continuous destination either).
    //
    // MIDI mapping (architecture.md MIDI Routing section: "fixed or
    // user-assignable MIDI CC/Note -> AudioParameterBool::setValueNotifyingHost()
    // (custom mapping table, no built-in JUCE MIDI-learn)"): NO per-trigger
    // MIDI-mapping parameter exists in the locked parameter-spec.md (same
    // "no sub-selector parameter exists" gap Phase 3.8 already documented
    // for the Mod Matrix's MIDI CC source and Macro source) -- a FIXED
    // Note-On/Note-Off mapping table (`kPerformanceTriggerMidiNotes`, notes
    // 36-42, the General-MIDI low-percussion/"finger-drum-pad" range, a
    // common convention for physical performance-trigger controllers) is
    // used instead, MOMENTARY semantics (Note-On -> true, Note-Off ->
    // false, matching "Momentary or toggle... recommend a simple internal
    // flag per trigger" per architecture.md component #11 -- Momentary
    // chosen as the simpler, more obviously "instant/dramatic,
    // hold-to-engage" live-performance feel; toggle behavior remains
    // available via the WebView UI / host automation regardless of this
    // MIDI mapping's own semantics, once Stage 3 adds the UI). Cached
    // `juce::RangedAudioParameter*` pointers (NOT `getRawParameterValue()`'s
    // read-only atomic<float>*) are required here since MIDI-driven writes
    // need the full parameter object's `setValueNotifyingHost()` API --
    // fetched once in the constructor (same caching justification/pattern
    // as Phase 3.8's 53 cached pointers: repeated string-keyed
    // `getParameter()` lookups inside a MIDI-message loop would be needless
    // repeated hashing). Extends Phase 3.8's EXISTING single MIDI scan loop
    // in `processBlock()` (one scan, not two) -- Note-On/Note-Off messages
    // are dispatched inside the SAME loop that already handles CC messages
    // (Mod Matrix's MIDI CC source + this phase's own XY-Pad MIDI CC
    // handling below), just a different `juce::MidiMessage` predicate
    // branch.
    //
    // ---- OVERSAMPLING / QUALITY ENGINE (component #12) ----
    // `qualityMode` (ECO/NORMAL/HIGH/EXTREME/AUTO) is ALREADY declared
    // (Stage 1). Factor mapping: ECO=1x (no oversampling instance used at
    // all -- the fast, zero-overhead path), NORMAL=2x, HIGH=4x, EXTREME=8x.
    // Three `juce::dsp::Oversampling<float>` instances are PREALLOCATED in
    // `prepareToPlay()` (one each for 2x/4x/8x -- `oversamplers[0..2]`) and
    // SWITCHED BETWEEN at runtime (per the task's explicit constraint:
    // "preallocate all factor variants... switch between them, documenting
    // the memory tradeoff") -- REJECTED the alternative architecture.md
    // itself suggests ("message-thread-triggered prepare() call on a NEW
    // instance, atomic pointer swap") as unnecessary added complexity
    // (cross-thread pointer-swap machinery) given the simpler preallocate-
    // all-variants approach already satisfies both "never resize/reallocate
    // the currently-in-use instance from the audio thread" (each variant's
    // OWN instance is never touched while another is active) and avoids any
    // new cross-thread communication mechanism.
    //
    // MEMORY TRADEOFF (explicitly flagged per the task's instruction):
    // three simultaneously-live `Oversampling<float>` instances (2x+4x+8x)
    // cost roughly 2+4+8=14x a single stereo sample's worth of internal
    // FIR/IIR polyphase filter state and staging buffers, ALL held for the
    // lifetime of the plugin regardless of which `qualityMode` is currently
    // selected -- a modest, fixed, small RAM cost (each instance is
    // `initProcessing(1)`-sized, see below, NOT `initProcessing(samplesPerBlock)`
    // -- further shrinking this footprint since only a 1-sample-per-channel
    // staging block is ever needed, see "PER-SAMPLE OVERSAMPLING CALLS"
    // below) traded for ZERO runtime allocation/re-`prepare()`/pointer-swap
    // complexity on any `qualityMode` change.
    //
    // CLICK-SAFETY ON RUNTIME FACTOR SWITCHING (explicitly flagged): when
    // the resolved factor changes block-to-block (detected via
    // `lastActiveOversamplingFactor`, matching this file's established
    // change-detection convention), the NEWLY-activated instance's internal
    // filter state reflects whatever it was doing the LAST time it was
    // active (potentially stale/silent for a long time) -- this
    // implementation calls `.reset()` on the newly-active instance at the
    // moment of the switch (allocation-free, bounded, real-time-safe),
    // clearing it to a clean zero rather than leaving spurious old energy
    // in its filters. This is CLICK-SAFE (bounded, deterministic) but NOT
    // perfectly glitch-free (a full crossfade between two simultaneously-
    // live oversampled streams would eliminate even that residual
    // transient, at meaningfully higher CPU/complexity cost) -- an accepted
    // MVP tradeoff since `qualityMode` changes are an infrequent, deliberate
    // user action (not something expected to be automated every block),
    // analogous to (and no worse than) this file's existing
    // `lastFilterTypeIndex`-triggered filter `.reset()` on `filterType`
    // changes.
    //
    // LATENCY REPORTING (architecture.md, "CRITICAL... easy to forget"):
    // `setLatencySamples()` is called ONLY when the resolved factor
    // actually changes (not every block -- `AudioProcessor::setLatencySamples()`
    // internally notifies the host via `updateHostDisplay()`, which is
    // heavier-weight than a lock-free atomic store; calling it only on a
    // genuine, infrequent user-initiated quality-mode change -- rather than
    // every block -- is the standard, accepted JUCE pattern for
    // dynamically-latent plugins and keeps this off the audio thread's hot
    // path). ECO (1x) reports 0 latency. `useIntegerLatency=true` is passed
    // to every `Oversampling` instance specifically so `getLatencyInSamples()`
    // returns a clean whole-sample value for `setLatencySamples()` (an int
    // API) with no truncation ambiguity.
    //
    // AUTO HEURISTIC (architecture.md's own admission: "the vaguest-
    // specified parameter... needs a concrete decision rule" -- FLAGGED,
    // this implementation's own resolution, per plan.md's own recommended
    // "scale oversampling factor with current Drive/Fold intensity"
    // approach): computed from the ALREADY-modulated, current-block
    // `phase32DriveModulationObservation` (post- Sequencer/Mod-Matrix/
    // Macro/Performance-Trigger drive value, not the raw base parameter --
    // "current" intensity, matching plan.md's own wording) and
    // `distortionFoldPct` (likewise already-modulated), normalized and
    // averaged into a single 0-1 `intensity` value, then quantized into one
    // of the 4 factors via fixed thresholds (`kAutoOversamplingThresholds`).
    // KNOWN LIMITATION (flagged): since `drive`/`fold` can themselves be
    // continuously modulated (LFOs, Mod Matrix, etc.), AUTO's resolved
    // factor can in principle change every block, which -- combined with
    // the click-safety `.reset()` above -- could produce audible flutter
    // under heavy modulation of those two specific parameters. No hysteresis/
    // smoothing is applied to AUTO's factor selection in this MVP (an
    // explicitly deferred future improvement, noted in the JSON report) --
    // AUTO mode's own underspecified nature (architecture.md's own words)
    // makes this an acceptable scope boundary for this phase rather than a
    // regression against a previously-more-precise spec.
    //
    // PER-SAMPLE OVERSAMPLING CALLS (the core restructuring, HIGHEST-RISK
    // integration of this phase -- see `processOversampledDistortionAndBitcrush()`'s
    // own doc comment at its definition in PluginProcessor.cpp for the full
    // causality argument): `juce::dsp::Oversampling::processSamplesUp()`/
    // `processSamplesDown()` are called with a 1-SAMPLE-PER-CHANNEL block,
    // ONCE PER BASE-RATE SAMPLE (not once per host block), rather than
    // batching the whole block through Oversampling in one call. This is a
    // deliberate, FLAGGED deviation from the "efficient" batched-block
    // oversampling pattern most examples show, REQUIRED because the
    // Feedback Routing Path's delay line can have a delay time (`microDelayTime`,
    // 0.1-50ms) SHORTER than a single host block -- meaning sample N's
    // Distortion-Engine input can depend on sample (N - delaySamples)'s
    // FULLY-COMPUTED (through Filter Stage) feedback output from EARLIER
    // IN THE SAME BLOCK, a genuine sample-accurate causal dependency that a
    // single batched up-sample-whole-block-then-process-then-down-sample
    // pass cannot express without either breaking that causality or
    // requiring a much more complex two-pass/lookahead buffering scheme.
    // Per-sample `Oversampling` calls are numerically CORRECT (JUCE's
    // internal FIR/IIR polyphase filters are continuous, causal filters --
    // processing them 1 sample at a time produces bit-identical output to
    // processing a whole block at once, as long as samples are never
    // skipped or reordered between calls) and fully real-time-safe
    // (`processSamplesUp()`/`processSamplesDown()` never allocate once
    // `initProcessing()` has run) -- the tradeoff is purely CPU overhead
    // (more function-call/loop overhead than a single batched call per
    // block), an ACCEPTED, EXPLICITLY-FLAGGED cost given architecture.md
    // ALREADY anticipates Oversampling as the plugin's single highest CPU
    // cost at EXTREME/8x regardless of batching strategy, and given
    // correctness (preserving the Feedback Routing Path's exact existing
    // causal semantics, per this phase's own "do not silently change the
    // rate at which any existing per-sample bookkeeping advances" task
    // constraint) is prioritized over raw efficiency for this MVP. A future
    // optimization pass could investigate batching within a single
    // delay-line-length's worth of samples if profiling shows this is a
    // genuine bottleneck (noted in the JSON report).
    //
    // SCOPE: EXACTLY Distortion Engine (#2) + Bitcrusher/SRR (#3) are
    // wrapped, per architecture.md component #12's explicit scope list
    // ("Distortion Engine, Bitcrusher/SRR, and the nonlinear elements
    // inside Feedback Routing -- NOT Glitch... or Filter/Sequencer/Mod
    // Matrix"). FLAGGED RESOLUTION for "the nonlinear elements inside
    // Feedback Routing": Phase 3.4's own tap-point resolution already
    // established that the loop's actual implemented "copy of the Filter
    // stage" is a LINEAR one-pole damping filter (not a second nonlinear
    // waveshaper), and its RMS-envelope limiter/tanh gain-clamp operate on
    // the ALREADY-post-Filter-Stage (base-rate, by architecture's own
    // explicit exclusion) signal -- there is no SEPARATE nonlinear
    // audio-rate waveshaping stage inside the feedback loop distinct from
    // Distortion Engine itself. Architecture's Integration Points section
    // clarifies the INTENT: Distortion + Bitcrush + "the feedback loop's
    // own nonlinear content" must be "processed inside the SAME oversampled
    // block... to avoid phase mismatches between the feedback-forward and
    // feedback-return paths." This implementation satisfies that intent
    // STRUCTURALLY: the feedback tap's recirculated sample
    // (`delayedFeedback`, popped from the delay line) is summed into the
    // Distortion Engine's input (`s = dry + delayedFeedback`) BEFORE the
    // up-sample boundary (see the restructured per-sample loop), so the
    // recirculated feedback content is processed THROUGH THE SAME
    // oversampled Distortion+Bitcrush pass as the forward-path signal --
    // no second, separate `Oversampling` instance wrapping the feedback
    // loop's own damping/soft-clamp/RMS-limiter machinery is needed or
    // built (that machinery correctly stays at base rate, consistent with
    // Filter Stage's explicit exclusion, since it operates strictly
    // downstream of Filter Stage's real, base-rate output).
    //
    // RATE-DEPENDENT INTERNAL STATE (Distortion Engine's tone-tilt filter /
    // DC blocker, Bitcrusher's SRR hold counter, Ring-Mod's carrier
    // oscillator) now run at up to 8x the base sample rate when oversampling
    // is active, and their existing single-scalar coefficients (computed
    // once in `prepareToPlay()` against the BASE rate only) would be WRONG
    // at an oversampled rate. Resolved by precomputing FOUR variants (one
    // per factor: 1x/2x/4x/8x) of each rate-dependent coefficient in
    // `prepareToPlay()` (`distortionToneLpCoeffByFactor`/
    // `distortionDcBlockerRByFactor`, indexed by `activeOversamplingFactorIndex`
    // each block) -- zero per-block trig/exp recomputation needed, just an
    // array lookup. Bitcrusher's SRR hold-counter is handled via a
    // per-block-computed `bitcrushEffectiveHoldSamples = bitcrushHoldSamples
    // * activeOversamplingFactor` (holding for N samples of the OVERSAMPLED
    // stream reproduces the SAME musical/audible hold duration relative to
    // the base rate that `sampleRateReduction`'s own units describe -- the
    // bitcrush QUANTIZER itself, `round(x*levels)/levels`, has no rate
    // dependency at all and needs no per-factor variant). Ring-Mod's
    // `juce::dsp::Oscillator` carrier is re-`prepare()`d once per block at
    // the current oversampled rate (`ringModOsc[ch].prepare(...)` -- cheap/
    // allocation-free per JUCE's documented `Oscillator::prepare()`
    // behavior, which only updates an internally-stored sample-rate value
    // used by `setFrequency()`'s phase-increment math -- it does NOT
    // regenerate the lookup table baked once by `initialise()` in
    // `prepareToPlay()`).
    //
    // driveGainSmoothed/distortionMixSmoothed still tick EXACTLY ONCE per
    // BASE-RATE sample (unchanged from Phase 3.3/3.4 -- see the outer
    // per-sample loop) -- their single resulting value for this base
    // sample is held CONSTANT across all `factor` oversampled sub-samples
    // within `processOversampledDistortionAndBitcrush()`'s inner loop, per
    // this phase's explicit "do not change the rate at which existing
    // per-sample bookkeeping (SmoothedValue ramps) advances" constraint --
    // no interpolation across oversampled sub-samples is performed for
    // these two values (their smoothing resolution was already coarser
    // than per-sample; holding constant across a handful of oversampled
    // sub-samples introduces no audible discontinuity).
    //
    // Real-time safety (summary): `Oversampling::initProcessing()`/
    // constructor allocation happens ONLY in `prepareToPlay()`.
    // `processSamplesUp()`/`processSamplesDown()`/`.reset()` are all
    // documented allocation-free. `oversampleScratchBuffer` is a
    // preallocated `juce::AudioBuffer<float>` (sized `numChannels x 1` in
    // `prepareToPlay()`, never resized in `processBlock()`). The one
    // deliberate exception to "no host-notification calls on the audio
    // thread" is `setLatencySamples()`, called only on an actual factor
    // change (see "LATENCY REPORTING" above) -- explicitly flagged, not an
    // oversight.
    //=========================================================================
    juce::SmoothedValue<float> xyPadXSmoothed, xyPadYSmoothed; // 0-100 range, matching xyPadX/xyPadY's own %-range
    float lastXyPadSmoothingMs = -1.0f; // change-detection sentinel (see "RAMP-LENGTH CHANGE HANDLING" above); -1 forces a clean first-block init
    juce::RangedAudioParameter* xyPadXParamForMidi = nullptr; // cached write-capable pointer (MIDI CC -> setValueNotifyingHost()), separate from the plain getRawParameterValue() read used for the glide target each block
    juce::RangedAudioParameter* xyPadYParamForMidi = nullptr;
    static constexpr int kXyPadMidiCcX = 2; // FLAGGED fixed CC (CC1 already claimed by Mod Matrix's MIDI CC source, Phase 3.8's kModMatrixMidiCcNumber)
    static constexpr int kXyPadMidiCcY = 3;
    static constexpr float kXyPadModDepthFraction = 0.5f; // reuses kSequencerModDepthFraction/kModMatrixDepthFraction's established half-range-swing convention (see "CONTRIBUTION DEPTH" doc comment above)

    static constexpr int kNumPerformanceTriggers = 7;
    std::array<juce::RangedAudioParameter*, kNumPerformanceTriggers> performanceTriggerParams {}; // cached write-capable pointers, order matches kPerformanceTriggerIds/kPerformanceTriggerMidiNotes below
    static constexpr std::array<const char*, kNumPerformanceTriggers> kPerformanceTriggerIds {
        "performanceKill", "performanceGlitch", "performanceDestroy", "performanceFreeze",
        "performanceReverse", "performanceStutter", "performanceChaos"
    };
    static constexpr std::array<int, kNumPerformanceTriggers> kPerformanceTriggerMidiNotes { 36, 37, 38, 39, 40, 41, 42 }; // FLAGGED fixed table -- see "MIDI mapping" doc comment above

    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 3> oversamplers; // index 0=2x(NORMAL), 1=4x(HIGH), 2=8x(EXTREME); ECO(1x)/AUTO-resolved-to-1x use no instance at all
    int activeOversamplingFactor = 1;        // 1/2/4/8, resolved once per block (see "AUTO HEURISTIC" above)
    int activeOversamplingFactorIndex = 0;   // 0=1x,1=2x,2=4x,3=8x -- indexes distortionToneLpCoeffByFactor/distortionDcBlockerRByFactor
    int lastActiveOversamplingFactor = -1;   // change-detection sentinel (see "CLICK-SAFETY" above); -1 forces a clean first-block resolve + initial setLatencySamples() call
    juce::AudioBuffer<float> oversampleScratchBuffer; // numChannels x 1, preallocated in prepareToPlay -- see "PER-SAMPLE OVERSAMPLING CALLS" above
    std::array<float, 4> distortionToneLpCoeffByFactor { 0.0f, 0.0f, 0.0f, 0.0f };   // index matches activeOversamplingFactorIndex (1x/2x/4x/8x)
    std::array<float, 4> distortionDcBlockerRByFactor { 0.0f, 0.0f, 0.0f, 0.0f };
    int bitcrushEffectiveHoldSamples = 1; // bitcrushHoldSamples * activeOversamplingFactor, recomputed once per block
    static constexpr std::array<float, 4> kAutoOversamplingThresholds { 0.25f, 0.5f, 0.75f, 1.0f }; // intensity breakpoints -> 1x/2x/4x/8x (see "AUTO HEURISTIC" above)

    void processOversampledDistortionAndBitcrush(const float* inputPerChannel, float* outputPerChannel,
                                                   int numChannelsThisCall, float driveGain, float distortionMixAmt);

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
    // AS OF PHASE 3.9: the tone-tilt filter / DC-blocker one-pole
    // coefficients are no longer single scalars — Distortion Engine now
    // runs at up to 8x the base rate inside the Oversampling wrapper, so
    // FOUR per-factor variants are precomputed in prepareToPlay instead
    // (`distortionToneLpCoeffByFactor`/`distortionDcBlockerRByFactor`, see
    // the Phase 3.9 doc comment's "RATE-DEPENDENT INTERNAL STATE" section,
    // indexed by `activeOversamplingFactorIndex`).
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
    juce::SmoothedValue<float> bitcrushLevelsSmoothed; // audit fix: click-free level ramps (Destroy trigger)
    float bitcrushLevelsCurrent = 65535.0f;
    // v3 addition: selectable quantize character (see processBitcrusher).
    // Dedicated fixed-seed RNG for Dither mode - never touches glitchRandom.
    int bitcrushModeIndex = 0;
    juce::Random bitcrushDitherRandom { 0x5EEDC0DE };
    int bitcrushHoldSamples = 1; // in units of BASE-rate samples (sampleRateReduction's own units); AS OF PHASE 3.9, processBitcrusher() actually holds for `bitcrushEffectiveHoldSamples` (this value * activeOversamplingFactor) since it now runs at the oversampled rate — see the Phase 3.9 doc comment's "RATE-DEPENDENT INTERNAL STATE" section.

    float processBitcrusher(float xIn, int channel);

    //=========================================================================
    // Stage 2 Phase 3.5: Glitch / Buffer Engine (18 Modes) — Tier 1 (Simple
    // Modes) — architecture.md component #4, plan.md's "Phase 3.5: Glitch
    // Engine - Tier 1 (Simple Modes)".
    //
    // Positioned between Bitcrusher/SRR (#3) and Filter Stage (#6) in the
    // per-sample chain, per the Processing Chain ASCII diagram and
    // Sequential DSP chain step 8 (Glitch runs on Bitcrusher's OUTPUT,
    // feeds the Filter Stage's INPUT — at the time this phase was written
    // (3.5), the Oversampling bracket around Distortion+Bitcrush was not
    // yet built, so Glitch simply followed Bitcrusher's real output
    // directly. AS OF PHASE 3.9: the Oversampling wrapper now brackets
    // Distortion+Bitcrush internally (up-sample -> Distortion -> Bitcrush
    // -> down-sample, all inside `processOversampledDistortionAndBitcrush()`),
    // but Glitch/Filter/Sequencer/Mod-Matrix stay explicitly OUTSIDE that
    // bracket per architecture.md's own scope list — Glitch still reads
    // Bitcrusher's (now down-sampled-back-to-base-rate) real output
    // directly, unchanged from this phase's original design).
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

    // --- Component #6: Filter Stage — Ableton Auto Filter style (v12) ---
    // Hand-rolled Cytomic/Zavalishin TPT state-variable filter: one process
    // step yields LP, BP, HP (and Notch = LP+HP) simultaneously, which is
    // what continuous Morph (LP->BP->HP->Notch) needs. Two stages in series
    // give the 24 dB/oct slope. Drive adds analog-style tanh saturation into
    // the filter input (Auto Filter's circuit character, approximated).
    // Types: 0 Lowpass, 1 Highpass, 2 Bandpass, 3 Notch, 4 Morph.
    struct TptSvf
    {
        float ic1eq = 0.0f, ic2eq = 0.0f;
        float g = 0.0f, k = 2.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        void setCoeffs (float cutoffHz, double sr, float kIn) noexcept
        {
            g  = std::tan (juce::MathConstants<float>::pi * cutoffHz / (float) sr);
            k  = kIn; // 1/Q
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        void reset() noexcept { ic1eq = ic2eq = 0.0f; }
        // Returns lp/bp/hp by reference; caller derives notch = lp + hp.
        inline void process (float v0, float& lp, float& bp, float& hp) noexcept
        {
            const float v3 = v0 - ic2eq;
            const float v1 = a1 * ic1eq + a2 * v3;
            const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
            ic1eq = 2.0f * v1 - ic1eq;
            ic2eq = 2.0f * v2 - ic2eq;
            lp = v2; bp = v1; hp = v0 - k * v1 - v2;
        }
    };
    std::array<TptSvf, 2> filterStage1; // per channel (12 dB)
    std::array<TptSvf, 2> filterStage2; // per channel, series at 24 dB

    int   filterTypeIndex  = 0;   // 0 LP, 1 HP, 2 BP, 3 Notch, 4 Morph
    int   filterSlopeIndex = 0;   // 0 = 12 dB, 1 = 24 dB
    float filterMorphNorm  = 0.0f; // 0..1 Morph position (LP->BP->HP->Notch)
    float filterDriveGain   = 1.0f; // linear pre-filter drive
    float filterDriveComp    = 1.0f; // makeup so drive doesn't just raise level
    int   lastFilterTypeIndex  = -1;
    int   lastFilterSlopeIndex = -1;
    bool  filterBypassed = false;

    void updateFilterParameters(float cutoffHz, float resonancePct, double sampleRate);
    float processFilterStage(float xIn, int channel);
    static inline float filterPickOutput (int type, float morphN, float lp, float bp, float hp) noexcept
    {
        switch (type)
        {
            case 0: return lp;
            case 1: return hp;
            case 2: return bp;
            case 3: return lp + hp;                 // notch
            default:                                 // 4 = Morph: LP->BP->HP->Notch
            {
                const float notch = lp + hp;
                const float p = juce::jlimit (0.0f, 1.0f, morphN) * 3.0f; // 0..3
                if (p < 1.0f) return lp  + (bp    - lp) * p;
                if (p < 2.0f) return bp  + (hp    - bp) * (p - 1.0f);
                return                hp  + (notch - hp) * (p - 2.0f);
            }
        }
    }

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
    float limiterAutoReleaseSmoothedMs = 300.0f; // v7: auto-release smoothing state
    juce::dsp::Gain<float> limiterCeilingPostGain; // v7: +ceiling dB after the limiter (see processBlock comment)
    juce::SmoothedValue<float> stereoWidthSmoothed; // audit fix: macroWidth's real M/S width (1.0 = neutral)
    // v9: auto gain balancer state (see processBlock's balancer stage)
    float autoGainDryEnv = 0.0f, autoGainWetEnv = 0.0f;
    juce::SmoothedValue<float> autoGainSmoothed;

    //=========================================================================
    // Stage 3 Phase 5.6: GUI Visualization Taps — atomic storage. See the
    // public getters above (just below getAPVTS()) for the full doc comment;
    // this is purely the backing storage. Written once per block from
    // processBlock()/resolveModMatrixAndMacroContributions()/
    // updateSequencerStepAndContributions() at the exact point each mirrored
    // quantity is already computed for real DSP use — see each store call
    // site's own inline comment in PluginProcessor.cpp.
    //=========================================================================
    std::atomic<float> visInputPeakLevel { 0.0f };
    std::atomic<float> visOutputPeakLevel { 0.0f };
    std::atomic<int>   visSequencerStepIndex { 0 };
    std::atomic<float> visModFoldPct { 0.0f };
    std::atomic<float> visModBitDepth { 16.0f };
    std::atomic<float> visModFilterCutoffHz { 20000.0f };
    std::atomic<float> visModMixPct { 100.0f };
    std::atomic<float> visModFeedbackAmountPct { 0.0f };
    std::atomic<float> visModGlitchProbabilityPct { 0.0f };

    juce::AudioProcessorValueTreeState parameters;

    // Parameter layout creation
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessor)
};
