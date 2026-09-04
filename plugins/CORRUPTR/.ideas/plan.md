# CORRUPTR - Implementation Plan

**Date:** 2026-09-01
**Complexity Score:** 5.0 (capped) — **uncapped raw score: 19.0** (Complex, far beyond standard "complex" threshold)
**Strategy:** MAJOR phase-based implementation — recommend treating this as multiple sequential implementation efforts, not a single phased Stage 3 pass

---

## Complexity Factors

- **Parameters:** ~85-90 estimated automatable parameters (46 explicit in `parameter-spec-draft.md` + ~40 architecture-required additions: per-LFO shape/sync ×4 LFOs, Mod Matrix 8 slots × 4 fields) → `min(86/5, 2.0)` = **2.0** (capped — already exceeded at just 10 parameters; actual count is ~8.6x the cap threshold)
- **Algorithms:** **14** DSP components counted from `architecture.md` Core Components:
  1. Input Gain Stage
  2. Distortion Engine (12-algorithm waveshaper)
  3. Bitcrusher / Sample Rate Reducer
  4. Glitch / Buffer Engine (18 modes)
  5. Rhythmic Sequencer
  6. Filter Stage (7 topologies)
  7. Feedback Routing Path
  8. Modulation Matrix + 4 LFOs
  9. Macro System (8 macros)
  10. XY Pad (with inertia)
  11. Performance Mode Triggers (7)
  12. Oversampling / Quality Engine
  13. Output Limiter (dual-mode)
  14. Master Mix Stage
- **Features:** **3** points
  - Feedback loops (+1) — explicit DISTORTION→FILTER→DELAY→DISTORTION path with safety limiting
  - Modulation systems (+1) — 4 LFOs, envelope follower, full mod matrix
  - External MIDI control (+1) — Performance Mode triggers, Mod Matrix MIDI CC source, optional XY Pad MIDI
  - (FFT/frequency-domain: 0 — no FFT-based processing in MVP scope; Multiband: 0 — explicitly Post-MVP)
- **Total (uncapped):** 2.0 + 14 + 3 = **19.0**
- **Total (capped per formula):** **5.0**

### Honest assessment of what the cap is hiding

The standard complexity formula caps at 5.0 specifically because most plugins that hit "complex" (score ≥ 3.0) are meaningfully described by a phased Stage 3 (3 DSP phases + 3 GUI phases). **CORRUPTR's uncapped score of 19.0 is roughly 4x the complexity of a plugin that just barely crosses the "complex" threshold (3.0), and roughly 6x the cap itself.** The cap exists to normalize scoring, not to imply CORRUPTR is only as complex as a typical score-5.0 plugin.

For calibration: a typical "complex" plugin in this codebase (e.g. a shimmer reverb with pitch-shifting + feedback + reverb) scores around 3-4 algorithms + 1-2 features + capped 2.0 params ≈ 6-8 uncapped. CORRUPTR's 14 algorithms alone (before adding features or params) already exceeds that. **This is not a "complex plugin" in the same sense the rest of the registry uses that word — it is a small DSP product (comparable in scope to iZotope Trash 2 or Sugar Bytes Effectrix, both explicitly researched above as professional precedents) being built as if it were a single plugin MVP.**

---

## Recommendation: Do Not Attempt This As One Continuous Stage 3 Pass

Every one of the 14 components above is individually buildable — none require research breakthroughs, all have either a direct JUCE class or a well-understood custom-implementation pattern (documented in `architecture.md`). The risk here is not "can this be built," it's **scope and sequencing risk**: attempting all 14 components plus their ~85 parameters plus a from-scratch UI (Distortion Graph routing view, macro rack, XY pad, sequencer grid, mod matrix editor, performance buttons, meters/visualization) in one continuous implementation effort would almost certainly produce:
- A half-working feedback safety mechanism validated too late (after other components already depend on it)
- Modulation accumulator bugs that are extremely hard to isolate once all four contributor layers (Sequencer/Mod Matrix/Macro/Performance) are wired in simultaneously
- A GUI mockup/build effort disconnected from a still-changing DSP surface (~40 parameters don't even exist in the current draft spec yet — LFO shape/sync, Mod Matrix slots)

**Recommended path:** Split into two explicit implementation waves rather than one large phased pass:

- **Wave 1 (v1.0 MVP-of-MVP):** A working, safe, sellable core — linear chain (Distortion, Bitcrush, Filter, Mix, Limiter) + safety-validated Feedback Routing + the SIMPLEST 6-8 Glitch modes + basic Sequencer (fewer lanes) + Macros + XY Pad + Performance Triggers, WITHOUT the full 8-slot free-form Mod Matrix or all 18 Glitch modes or Oversampling AUTO.
- **Wave 2 (v1.1+):** Full 18-mode Glitch Engine, full 10-lane/10-operation Sequencer, full 8-slot Mod Matrix, Oversampling AUTO heuristic, Colored Limiter refinement.

This is a **product/scope decision**, not purely a technical one — flagging it clearly here per the task's request rather than silently absorbing it into an oversized phase list. If the user/orchestrator wants to proceed with the FULL MVP as originally scoped in one continuous effort, the phase breakdown below still applies (it's the same underlying dependency order either way), but expect Stage 3 to span meaningfully more calendar time than a typical score-5.0 plugin in this registry.

---

## Stages

- Stage 0: Research ✓ (this document + architecture.md)
- Stage 1: Foundation ← Next
- Stage 2: Shell (APVTS parameter declarations — expect this alone to be a substantial task at ~85-90 parameters)
- Stage 3: DSP — phased, MAJOR (9 phases, detailed below; consider Wave 1/Wave 2 split above)
- Stage 3: GUI — phased, MAJOR (6+ phases, detailed below)
- Stage 3: Validation — presets, pluginval, changelog (expect an unusually large automated test matrix per the brief's own Testing Notes: per-algorithm, per-mode, feedback stability, NaN/Inf, sample-rate sweep, block-size sweep)

---

## Stage 3: DSP Phases

Ordering follows the "safety and foundation first" recommendation from `architecture.md`'s Overall Project Risk section — NOT simply top-to-bottom through the signal chain.

### Phase 3.1: Feedback Routing Safety Validation (Isolated)

**Goal:** Prove the feedback loop's soft-clamp + damping + circuit-breaker mechanism is stable BEFORE any other component is built around it.

**Components:**
- `juce::dsp::DelayLine` feedback delay
- Soft-clamp gain mapping (`tanh`-based, per `architecture.md` component #7)
- In-loop damping filter
- Per-block `std::isfinite()` circuit breaker

**Test Criteria:**
- [ ] Sine sweep + impulse test through isolated feedback loop shows bounded output RMS at ALL `feedbackAmount` × `feedbackDamping` × filter-resonance combinations
- [ ] Deliberately injected NaN/Inf is caught and recovered within one block, never propagates to output
- [ ] No audible runaway/blow-up at any parameter combination, including worst-case (max feedback, min damping, max filter resonance)

---

### Phase 3.2: Unified Modulation Accumulator (Isolated)

**Goal:** Build and validate the per-parameter modulation-summing mechanism (base + Sequencer + Mod Matrix + Macro + Performance Trigger override) with synthetic test inputs, before wiring it into real DSP.

**Components:**
- Modulation accumulator core (additive combine, Performance Trigger override priority, final range clamp)
- Stub/test sources feeding one destination (e.g. `drive`) end-to-end

**Test Criteria:**
- [ ] Base value + synthetic Sequencer + synthetic Mod Matrix + synthetic Macro all combine additively and correctly for a single test destination
- [ ] Performance Trigger override correctly wins over other contributions while active, correctly releases when inactive
- [ ] Combined result is always clamped within the destination parameter's valid range regardless of input combination

---

### Phase 3.3: Core Linear Chain

**Goal:** Working, non-modulated core signal path — Input Gain through Output Limiter, no Glitch/Sequencer/Mod Matrix/Macros yet.

**Components:**
- Input Gain
- Distortion Engine (all 12 algorithms)
- Bitcrusher / SRR
- Filter Stage (all 7 topologies)
- Master Mix (0-200%)
- Output Gain
- Output Limiter (both Transparent and Colored modes)

**Test Criteria:**
- [ ] Plugin loads in DAW without crashes, passes audio through cleanly at unity settings
- [ ] Each of the 12 distortion algorithms produces distinct, correct output
- [ ] Bitcrusher/SRR bypass cleanly at default (16-bit, factor 1) and audibly crush at extremes
- [ ] Each of the 7 filter types behaves correctly (frequency response spot-checked)
- [ ] Mix behaves correctly across full 0-200% range including >100% overdrive behavior
- [ ] Both limiter modes prevent clipping; Colored mode audibly different from Transparent

---

### Phase 3.4: Feedback Routing Integration

**Goal:** Wire the Phase 3.1-validated feedback loop into the Phase 3.3 core chain at its correct chain position (post-Filter, pre-Mix, feeding back into Distortion input).

**Components:**
- Feedback tap/sum points in the core chain
- Re-validation of safety properties now that real (not synthetic) Distortion/Filter processing is in the loop

**Test Criteria:**
- [ ] Feedback stability tests from Phase 3.1 still pass with real Distortion/Filter in the loop
- [ ] Feedback audibly and musically usable across its full range without instability

---

### Phase 3.5: Glitch Engine — Tier 1 (Simple Modes)

**Goal:** Ship the 6-8 simplest, highest-value Glitch modes first: Stutter, Repeat, Reverse, Freeze, Silence, Retrigger.

**Components:**
- Ring buffer (`juce::AudioBuffer` based)
- Fixed-rate read/write for the Tier 1 modes only

**Test Criteria:**
- [ ] Each Tier 1 mode produces expected, click-free buffer manipulation
- [ ] Mode switching mid-playback doesn't produce discontinuities (cross-fade validated)
- [ ] `glitchProbability` and `chaos` gate triggers correctly with reproducible (seeded) randomness

---

### Phase 3.6: Glitch Engine — Tier 2 (Advanced Modes)

**Goal:** Remaining 10-12 modes requiring variable-rate/interpolated reads: Buffer Repeat, Slice, Random Slice, Micro Loop, Granular Repeat, Tape Stop, Tape Start, Pitch Jump, Noise Burst, Bitcrush Burst, Random Repeat.

**Components:**
- `juce::dsp::DelayLine` (Lagrange3rd) fractional-rate reads
- Granular windowing (`juce::dsp::WindowingFunction`)

**Test Criteria:**
- [ ] Each Tier 2 mode produces expected output; Granular Repeat specifically checked for CPU cost
- [ ] Tape Stop/Start ramps sound musically correct (no clicks at ramp start/end)
- [ ] Pitch Jump produces correct musical intervals

---

### Phase 3.7: Rhythmic Sequencer

**Goal:** Host-synced step sequencer with all 10 modulation lanes and pattern operations, wired into the Phase 3.2 modulation accumulator.

**Components:**
- `juce::AudioPlayHead::getPosition()` host sync
- 10-lane pattern `ValueTree` state + double-buffered audio-thread snapshot
- Pattern operations (Random/Mutate/Reverse/Mirror/Shift/Half/Double/Syncopate/Clear/Humanize)

**Test Criteria:**
- [ ] Sequencer steps advance correctly synced to host tempo/PPQ at all 7 rate divisions
- [ ] No-transport fallback (freeze at step 0) verified in Standalone with no playback
- [ ] Each pattern operation produces correct, expected pattern transformation
- [ ] Sequencer output correctly reaches destinations via the modulation accumulator

---

### Phase 3.8: Modulation Matrix + 4 LFOs + Macros

**Goal:** Full free-routing Mod Matrix (8 slots, 8 sources, ~12 destinations) and all 8 Macros, both wired into the modulation accumulator alongside the already-integrated Sequencer.

**Components:**
- Custom 9-shape `Lfo` class ×4
- `juce::dsp::BallisticsFilter` (Envelope/Audio Level sources)
- 8-slot Mod Matrix (APVTS-backed)
- `MacroEngine` with fixed routing table

**Test Criteria:**
- [ ] Each of the 4 LFOs produces correct waveform for all 9 shapes, both free-Hz and tempo-synced
- [ ] Each Mod Matrix source correctly drives each destination when assigned
- [ ] Each of the 8 Macros produces its documented combined-parameter effect
- [ ] Sequencer + Mod Matrix + Macro simultaneously targeting the same destination combine correctly (regression test against Phase 3.2's synthetic validation, now with real sources)

---

### Phase 3.9: XY Pad, Performance Triggers, Oversampling/Quality Modes

**Goal:** Final layer — performance-instrument controls and the Quality/Oversampling engine.

**Components:**
- `SmoothedValue`-based XY Pad glide
- 7 Performance Mode triggers with MIDI mapping
- `juce::dsp::Oversampling` at ECO/NORMAL/HIGH/EXTREME + AUTO heuristic

**Test Criteria:**
- [ ] XY Pad glides smoothly toward drag targets per `xyPadSmoothing`, no zipper noise
- [ ] Each Performance Trigger produces its documented preset-diff override, correctly releases
- [ ] MIDI triggering works for Performance Mode buttons
- [ ] Each Quality mode produces audibly reduced aliasing at higher factors; latency correctly reported via `setLatencySamples()`
- [ ] AUTO mode behaves per whatever concrete heuristic was decided (flagged as a pre-Stage-3 product decision in `architecture.md`)

---

## Stage 3: GUI Phases

CORRUPTR's UI concept (Distortion Graph routing view, large XY pad, dedicated Glitch/Rhythm/Modulation/Filter/Output sections, waveform/spectrum visualization, modulation-range indicators on knobs) is itself a substantial build. No mockup exists yet — GUI phases below assume mockup work happens in parallel/after this Stage 0 pass per the "quick params + parallel workflow" path, and will need reconciliation once the mockup is finalized (particularly the ~40 architecture-added parameters not yet in the draft spec).

### Phase 5.1: Core Layout and Primary Controls

**Goal:** Basic WebView shell with Distortion/Filter/Output sections bound to Phase 3.3's core chain parameters.

**Test Criteria:**
- [ ] WebView loads, core Distortion/Filter/Output controls visible and bound
- [ ] Algorithm selector, Drive, Mix, Filter controls functional

---

### Phase 5.2: Glitch, Sequencer, and Rhythm Sections

**Goal:** Glitch mode selector, buffer length, and Sequencer step grid UI.

**Test Criteria:**
- [ ] Glitch mode selector and controls bound and functional
- [ ] Sequencer step grid renders and edits pattern data across all 10 lanes
- [ ] Pattern operation buttons (Random/Mutate/etc.) functional

---

### Phase 5.3: Modulation Matrix, LFOs, and Macros

**Goal:** Mod Matrix slot editor UI, LFO controls, Macro knob rack.

**Test Criteria:**
- [ ] Mod Matrix slots editable (source/destination/amount/polarity)
- [ ] LFO shape/rate/sync controls functional for all 4 LFOs
- [ ] Macro knobs bound and audibly affect their documented target parameters

---

### Phase 5.4: XY Pad and Performance Mode

**Goal:** Large XY pad with glide feel, 7 Performance Mode trigger buttons.

**Test Criteria:**
- [ ] XY pad streams `xyPadX`/`xyPadY` parameter updates CONTINUOUSLY during the drag gesture (pointermove-driven, with beginChangeGesture/endChangeGesture bracketing the drag) — values audibly change in real time while dragging, never only on release
- [ ] XY pad drag produces smooth glide (not snap) matching `xyPadSmoothing`
- [ ] Performance Mode buttons trigger correctly, visually indicate active state
- [ ] MIDI-triggered Performance Mode reflected in UI state

---

### Phase 5.5: Distortion Graph Visualization

**Goal:** The brief-flagged potential visual-identity feature — modular routing graph with connectable modulation links.

**Test Criteria:**
- [ ] Graph renders current signal-chain module states
- [ ] Modulation links between nodes visually represented
- [ ] Performance acceptable (no CPU spikes from graph rendering)

**Note:** This phase is explicitly the most speculative/least-specified GUI element (brief calls it "a potential major visual-identity feature... worth prioritizing in UI mockup exploration" rather than a locked requirement). Recommend validating feasibility during mockup work before committing to a full implementation here — could be simplified to a static (non-interactive) routing diagram if the connectable-links version proves too complex for the available time.

---

### Phase 5.6: Meters, Waveform/Spectrum Display, Modulation Range Indicators

**Goal:** Real-time visualization layer — input/output meters, waveform/spectrum, per-knob modulation-range rings.

**Test Criteria:**
- [ ] Meters update smoothly (ballistic motion, per `juce8-critical-patterns.md` pattern #20) without audio-thread interference
- [ ] Waveform/spectrum display renders in real time
- [ ] Modulated parameters visually show their modulation range around the knob

---

### Implementation Flow

- Stage 1: Foundation - project structure
- Stage 2: Shell - APVTS parameters (~85-90 parameters — expect this to be a large, careful task in its own right)
- Stage 3: DSP - 9 phases (3.1-3.9, safety/foundation-first ordering)
- Stage 3: GUI - 6 phases (5.1-5.6)
- Stage 3: Validation - presets (~15-20 per brief), pluginval, changelog, plus an expanded automated test matrix per the brief's own Testing Notes (per-algorithm, per-glitch-mode, feedback stability, NaN/Inf at all settings, mono/stereo, 44.1/48/88.2/96/192kHz, block-size sweep, automation, preset restore)

---

## Implementation Notes

### Thread Safety
- All parameter reads via atomic `getRawParameterValue()->load()` or the modulation accumulator's lock-free snapshot mechanism
- Sequencer pattern edits: double-buffered `ValueTree`, pointer swap (no locks in audio thread)
- No background thread needed — MVP has no file I/O

### Performance
- Brief's CPU target: ~3-5% per instance at default settings — this is a REAL constraint given 14 components; recommend per-component profiling during Stage 3 rather than only whole-chain profiling
- Highest-cost components: Oversampling at EXTREME/8x, Glitch Engine's Granular Repeat mode, Feedback Routing (processes Distortion+Filter twice per block)

### Known Challenges
- Feedback safety math must be proven correct before anything downstream depends on it (Phase 3.1 exists specifically to front-load this risk)
- Modulation accumulator correctness is the hardest-to-isolate bug category in this plugin — build and test it in isolation (Phase 3.2) before wiring in real sources
- ~40 parameters (LFO shape/sync, Mod Matrix slots) exist in `architecture.md` but NOT yet in `parameter-spec-draft.md` — reconcile before/during Stage 2 Shell, ideally alongside UI mockup finalization
- Several spec gaps flagged in `architecture.md`'s Notes section need product decisions before Stage 3: `distortionAlgorithm=Bitcrusher/SRR` behavior, feedback loop delay time, `qualityMode=AUTO` heuristic, `macroWidth`'s pre-Stereo-module behavior

---

## References

- Creative brief: `plugins/CORRUPTR/.ideas/creative-brief.md`
- Parameter spec (draft): `plugins/CORRUPTR/.ideas/parameter-spec-draft.md`
- DSP architecture: `plugins/CORRUPTR/.ideas/architecture.md`
- UI mockup: not yet created (deferred per "quick params + parallel workflow" path)

**Reference plugins in this registry:**
- Scatter (Granular Delay) — relevant precedent for buffer/grain-based processing patterns applicable to Glitch Engine's Granular Repeat mode
- DrumRoulette (multi-slot, custom ValueTree state, double-buffered cross-thread patterns) — direct precedent for Sequencer's pattern-data state management approach
- AutoClip (Hard Clipper) — smallest-scope precedent for `juce::dsp::WaveShaper`-based distortion algorithm implementation
