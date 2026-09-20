# DROP - Implementation Plan

**Date:** 2026-09-13
**Complexity Score:** 5.0 (Complex)
**Strategy:** Phase-based implementation

---

## Complexity Factors

- **Parameters:** 19 parameters (18 drafted + `digital` added per architecture.md's resolution of the open question) → 19/5 = 3.8, capped at 2.0 = **2.0**
- **Algorithms:** 17 DSP/core-component components (each a `### [Component]` subsection under architecture.md's Core Components) = **17**
  - MusicalClock, TransitionEngine, DropStateMachine, AudioAnalyzer, IntensityBudget, FilterModule, WidthModule, PitchModule, GateModule, StutterModule, DelayModule, ReverbModule, SaturationModule, DigitalModule, NoiseModule, VacuumModule, ImpactEngine
  - (DropStyle, DropRandomizer/Chaos, and Presets are documented under architecture.md's "Data-Driven Style System" — data/coordination, not per-sample DSP algorithms — and are intentionally NOT counted here, to keep the algorithm count honest to the formula's intent)
- **Features:** 4 points
  - FFT/frequency-domain processing (+1): AudioAnalyzer's 512-point `juce::dsp::FFT` spectral-centroid/band-energy analysis
  - Feedback loops (+1): DelayModule's feedback path (HPF/LPF + soft-clip safety)
  - Modulation systems (+1): TransitionEngine progress curves, GateModule/StutterModule tempo-synced pattern generators, ImpactEngine sub-hit AR envelope, VacuumModule collapse envelope — pervasive modulation-driven behavior throughout
  - External MIDI control (+1): configurable MIDI-note trigger routing
  - (Multiband processing NOT counted: WidthModule's low/high crossover split is a protection mechanism, not true parallel multiband effect processing)
- **Total:** 2.0 + 17 + 4 = 23.0, capped at **5.0**

This is a Tier 6 (real-time analysis + FFT + sample-accurate scheduling), maximum-complexity plugin by this formula. Strategy is unambiguously **phased implementation**.

---

## Stages

- Stage 0: Research ✓
- Stage 1: Planning ✓ (this document)
- Stage 1: Foundation ← Next
- Stage 2: Shell
- Stage 3: DSP — 5 phases (3.1-3.5)
- Stage 3: GUI — 3 phases (4.1-4.3)
- Stage 3: Validation

**Relationship to implementation-spec.md's own 10-phase DEVELOPMENT PROCESS:** the spec mandates its own phase list (Phase 1 repo-inspect/skeleton through Phase 10 validation/profiling), each with a hard "compile, test, fix, verify state, then continue" gate. That mandate is honored here, not replaced — the mapping is:

| Spec phase | This plan's stage/phase |
|---|---|
| Phase 1 (inspect repo, skeleton, parameters, build config) | Stage 1: Foundation + Stage 2: Shell |
| Phase 2 (MusicalClock + tests) | Stage 3 DSP Phase 3.1 |
| Phase 3 (TransitionEngine + state machine) | Stage 3 DSP Phase 3.1 |
| Phase 4 (core DSP: filter, width, gate, saturation, delay, reverb) | Stage 3 DSP Phase 3.2 |
| Phase 5 (creative DSP: stutter, pitch, digital, vacuum, noise) | Stage 3 DSP Phase 3.3 |
| Phase 6 (ImpactEngine) | Stage 3 DSP Phase 3.4 |
| Phase 7 (IntensityBudget, DropStyle, presets wiring) | Stage 3 DSP Phase 3.5 |
| Phase 8 (UI and visualization) | Stage 3 GUI Phase 4.1-4.3 |
| Phase 9 (complete preset library and parameter behavior) | Stage 3 Validation (preset polish sub-step) |
| Phase 10 (full validation, profiling, cleanup) | Stage 3 Validation |

**The compile-and-test gate is mandatory after every phase listed below, not optional** — this is a direct requirement from implementation-spec.md's DEVELOPMENT PROCESS section and is repeated in each phase's Test Criteria.

---

## Stage 3: DSP Phases

#### Phase 3.1: Timing & Control Skeleton (MusicalClock, TransitionEngine, DropStateMachine, Sample-Accurate Segmentation)

**Goal:** Build the deterministic timing/state foundation that every other DSP module depends on, and prove it correct in complete isolation before any audible DSP exists. This is the highest systemic-risk area (see architecture.md > Implementation Risks > Overall Project Risk) and must be solid before building on top of it.

**Components:**
- `Source/DSP/MusicalClock.*` — BPM/PPQ/time-sig read from `AudioPlayHead`, musical-division-to-sample-count conversion, standalone-app fallback transport.
- `Source/DSP/TransitionEngine.*` — Linear/Exponential/S-Curve/LateAcceleration/Custom progress curves.
- `Source/DSP/DropStateMachine.*` — Idle/Arming/Building/Peak/Impact/Recovery enum + transition table + re-trigger safety.
- Central block-segmentation loop in `PluginProcessor::processBlock()` (or a shared helper) implementing sub-block splitting at trigger/state-transition/Vacuum/Impact sample offsets.
- `trigger` parameter + MIDI-note trigger detection wired to Idle→Arming.

**Test Criteria:**
- [ ] Plugin loads in DAW/Standalone without crashes (still audibly silent/pass-through — no DSP modules exist yet, audio should pass through unmodified)
- [ ] Unit tests: MusicalClock division→sample conversion correct at multiple tempos/sample rates/time signatures
- [ ] Unit tests: each TransitionEngine curve type produces expected 0→1 shape at known progress fractions
- [ ] Unit tests: DropStateMachine transitions occur at exact expected sample offsets across varying host block sizes (1, 32, 64, 128, 512, 2048, 8192 samples per architecture.md's segmentation risk mitigation)
- [ ] Re-trigger safety verified: `trigger` toggled while state != Idle has no effect
- [ ] MIDI-note trigger fires the same Idle→Arming transition as the `trigger` parameter
- [ ] No NaN/Inf, no allocation/locks in `processBlock()` (verified by code review + debug-build assertions)
- [ ] Compile clean, all above tests pass, before proceeding to Phase 3.2

---

#### Phase 3.2: Core DSP Modules (Filter, Width, Gate, Saturation, Delay, Reverb)

**Goal:** Implement the six best-understood, JUCE-primitive-backed DSP modules and validate the "always-process, weight-mix" pattern end-to-end with real, listenable audio for the first time.

**Components:**
- `Source/DSP/FilterModule.*` — `juce::dsp::StateVariableTPTFilter`, HP/LP/BP/dual.
- `Source/DSP/WidthModule.*` — M/S encode/decode + low-band protection crossover.
- `Source/DSP/GateModule.*` — tempo-synced envelope generator with anti-click ramps.
- `Source/DSP/SaturationModule.*` — 5-mode waveshaping + selective `juce::dsp::Oversampling`.
- `Source/DSP/DelayModule.*` — `juce::dsp::DelayLine<Lagrange3rd>` + feedback-safe HPF/LPF/soft-clip.
- `Source/DSP/ReverbModule.*` — `juce::dsp::Reverb` (modern `prepare`/`process` API per juce8-critical-patterns.md #17).
- A minimal placeholder `IntensityBudget` (fixed/static weights, not yet Style-driven) sufficient to exercise the always-process/weight-mix pattern across these six modules.
- Fixed series processing order per architecture.md: Filter → Width → Gate → [Pitch/Stutter/Delay/Saturation series slot, Pitch/Stutter deferred to 3.3] → Delay → Saturation.

**Test Criteria:**
- [ ] Audio passes through cleanly at weight=0 for all six modules (bit-transparent or near-transparent, no audible artifact from "always processing")
- [ ] Each module's weight parameter smoothly scales its audible contribution from 0 to full
- [ ] FilterModule: no instability/blow-up at max resonance sweep; low-frequency floor respected
- [ ] WidthModule: low band stays protected (verify via correlation/mono-sum test) regardless of width weight
- [ ] GateModule: rhythmic edges are audible and in-tempo, no zipper/click at edges
- [ ] SaturationModule: all 5 modes stable (no NaN/blow-up) across full drive range; oversampling engages only above documented threshold
- [ ] DelayModule: feedback cannot runaway even with feedback parameter automated to extreme values rapidly (unit test + manual stress test)
- [ ] ReverbModule: uses modern `dsp::Reverb` API exclusively, CPU usage within estimated budget (~3-4%)
- [ ] No NaN/Inf/denormal issues across silence, DC, impulse, sine, noise, full-scale test signals (per spec's UNIT TESTS section)
- [ ] Compile clean, all tests pass, before proceeding to Phase 3.3

---

#### Phase 3.3: Creative DSP Modules (Stutter, Pitch, Digital, Vacuum, Noise)

**Goal:** Implement the remaining, higher-risk creative modules, including the two flagged HIGH-risk items (PitchModule, and VacuumModule's tight-timing click-free requirement).

**Components:**
- `Source/DSP/PitchModule.*` — granular/WSOLA time-domain pitch shifter (see architecture.md Algorithm Details deep-dive). Implement and unit-test in isolation FIRST (pure sine sweep + musical loop test) before wiring into the chain, per architecture.md's PitchModule mitigation strategy.
- `Source/DSP/StutterModule.*` — preallocated circular capture buffer, repeat/reverse/half/double/pitch-variation modes (pitch-variation mode reuses PitchModule's grain engine).
- `Source/DSP/DigitalModule.*` — bit/SR crush + DC blocker.
- `Source/DSP/VacuumModule.*` — sample-accurate raised-cosine collapse envelope, tested explicitly at extreme tempo (200+ BPM) per architecture.md's mitigation strategy.
- `Source/DSP/NoiseModule.*` — `std::mt19937`-seeded white/pink/filtered noise (NOT `juce::Random`, per determinism requirement).
- `Source/Core/DropRandomizer.*` — Chaos seed management, musically-constrained randomization tables, Arming-time RNG draw scheduling.

**Test Criteria:**
- [ ] PitchModule isolated unit test: known semitone shift verified via FFT-based pitch measurement on a test sine sweep
- [ ] PitchModule integrated: no unacceptable grain-boundary artifacts on musical test material at all 7 semitone steps (Rise/Fall/Rise+Fall modes)
- [ ] StutterModule: no discontinuities/clicks at any repeat-boundary crossfade, across all 5 modes
- [ ] VacuumModule: envelope verified click-free (no discontinuity/derivative jump) via automated signal analysis, including at 200+ BPM edge case
- [ ] NoiseModule + DropRandomizer: same seed + same inputs reproduces bit-identical output across two separate runs (determinism test)
- [ ] DigitalModule: bit-crush/SR-reduction stable, no DC buildup (DC blocker verified)
- [ ] All modules preallocate buffers in `prepareToPlay()` only — zero allocation verified in `processBlock()` (debug-build allocation-detection assertions)
- [ ] Compile clean, all tests pass, before proceeding to Phase 3.4

---

#### Phase 3.4: ImpactEngine

**Goal:** Implement the coordinator module that ties together the signature Vacuum→Impact moment, emphasizing the original source transient.

**Components:**
- `Source/DSP/ImpactEngine.*` — sub-hit sine oscillator (40-80Hz, band-limited, AR-enveloped), noise/transient burst (reuses NoiseModule generator), stereo-expansion drive into WidthModule, light SaturationModule pass, short-tail ReverbModule instance, low-end soft-clip safety.
- Wiring of DropStateMachine's Peak/Impact state gating to activate VacuumModule (Phase 3.3) immediately followed by ImpactEngine.

**Test Criteria:**
- [ ] Source transient audibly preserved through Vacuum→Impact (A/B test: dry transient vs. processed — the original hit should still be clearly recognizable, per spec's "Impact should emphasize the original musical transient")
- [ ] Sub-hit stays within 40-80Hz target band, fast attack/short decay confirmed
- [ ] Low-end controlled (soft-clip verified, no muddy buildup) via test with bass-heavy source material
- [ ] Vacuum ramp-down/ImpactEngine attack coordinate with no double-envelope bump (verified by ear + signal analysis)
- [ ] `impact` parameter scales sub/burst/expansion/tail strength smoothly across its full range
- [ ] Short tail does not become "an indistinct wash" (verify tail decay length stays within Style-configured bounds)
- [ ] Compile clean, all tests pass, before proceeding to Phase 3.5

---

#### Phase 3.5: IntensityBudget, DropStyle, AudioAnalyzer, and Preset Wiring

**Goal:** Replace the Phase 3.2 placeholder budget with the real, Style-driven, source-aware, Chaos-jittered IntensityBudget, and wire up all 16 DropStyles.

**Components:**
- `Source/DSP/AudioAnalyzer.*` — RMS/peak/transient-energy/spectral-centroid (512-pt FFT)/band-energy/stereo-correlation, `SourceProfile` classification.
- `Source/DSP/IntensityBudget.*` — full weighted-and-capped-proportional normalization, Style curves × macros × intensity/tension × source-awareness × Chaos jitter.
- `Source/Core/DropStyle.*` — all 16 style data tables (weight curves, curve type, timing prefs, source-awareness scalars, low-frequency-protection flags, Digital/Chaos activation curves).
- `Source/Core/DropPreset.*` — preset list = the 16 DropStyles at recommended macro defaults.
- Full 19-parameter APVTS wiring finalized (if not already complete from Stage 2: Shell) including the `digital` macro.

**Test Criteria:**
- [ ] `computeWeights(...)` pure function unit-tested: `sum(output) <= totalBudget(progress)` invariant holds across randomized fuzz inputs
- [ ] Source-aware adjustment verified: vocal-classified input measurably shifts weight distribution toward Pitch/Delay/Reverb/Width vs. drum-classified input shifting toward Gate/Stutter/Digital
- [ ] All 16 Styles produce audibly distinct transition behavior (not just different static parameter values) on the same test loop
- [ ] Chaos determinism verified end-to-end: same Style+seed+source+transport reproduces bit-identical output
- [ ] `impact` confirmed NOT part of the shared budget pool (raising other macros never reduces available impact headroom, and vice versa)
- [ ] AudioAnalyzer CPU cost within estimated budget (~1-2%), FFT hop-rate decoupled correctly from variable host block sizes
- [ ] Compile clean, all tests pass — Stage 3 DSP complete, proceed to Stage 3 GUI

---

## Stage 3: GUI Phases

#### Phase 4.1: Layout and Basic Controls

**Goal:** Integrate the DROP UI mockup (once created — note: no mockup exists as of this plan; Stage 2/GUI mockup work happens in parallel per creative-brief.md's Next Steps) and bind primary macro-row parameters.

**Components:**
- Copy finalized mockup HTML/CSS/JS to `Source/ui/public/` (WebView-based UI, per repo convention).
- `Source/UI/DropEditor.*`, `Source/UI/DropButton.*` — large DROP trigger button, macro row (Build, Style, Intensity, Tension, Chaos, Impact, Dry/Wet, Output).
- CMakeLists.txt WebView resource configuration (`NEEDS_WEB_BROWSER TRUE`, binary data registration including `check_native_interop.js` per juce8-critical-patterns.md #9/#13).
- Collapsible advanced row (Filter, Width, Pitch, Gate, Stutter, Delay, Reverb, Distortion, Digital, Noise).

**Test Criteria:**
- [ ] WebView window opens at correct default size
- [ ] DROP button and macro row render and match mockup design
- [ ] Advanced row correctly collapses/expands
- [ ] No DSP execution anywhere in UI/paint code (verified by code review, per spec's "UI must never execute DSP")

---

#### Phase 4.2: Parameter Binding and Interaction

**Goal:** Two-way parameter communication (UI ↔ DSP) for all 19 parameters, MIDI-trigger-note configuration UI, and gesture-correct automation.

**Components:**
- `WebSliderRelay`/`WebToggleButtonRelay`/`WebComboBoxRelay` bindings for all 19 parameters (JUCE 8 three-argument `WebSliderParameterAttachment` constructor per juce8-critical-patterns.md #12).
- Automation gesture bracketing (`sliderDragStarted`/`sliderDragEnded`) on every interactive control per juce8-critical-patterns.md #23.
- Trigger-mode selector (Manual/MIDI/Auto-sync) + MIDI-note picker UI, writing to non-parameter ValueTree state (not APVTS).
- Style selector UI reflecting the 16 DropStyle names.

**Test Criteria:**
- [ ] Every control movement changes the corresponding DSP parameter/state correctly
- [ ] Host automation of all 19 parameters updates UI controls correctly
- [ ] DROP button press correctly triggers Idle→Arming (respecting re-trigger safety — button has no effect mid-transition)
- [ ] MIDI-trigger-note configuration persists correctly across save/reload
- [ ] Automation can be recorded via touch/latch modes in at least one DAW (Ableton preferred, per repo's documented gesture-bracketing gotcha)
- [ ] No lag/visual glitches during rapid parameter changes

---

#### Phase 4.3: Visualization (DropGraph)

**Goal:** Real-time transition graph — tension over time, drop point, transition position, input level, active effects — driven by the lock-free audio→GUI FIFO from architecture.md.

**Components:**
- `Source/UI/DropGraph.*` — renders progress/tension/state/RMS/active-module-weights.
- `Source/UI/MeterDisplay.*` — input level meter.
- Lock-free ring-buffer drain via `juce::Timer` (~30-60Hz), ballistic/smoothed visual interpolation per juce8-critical-patterns.md #20 (separate current/target values, requestAnimationFrame-style loop if implemented in WebView JS, or JUCE Timer-driven repaint if native).

**Test Criteria:**
- [ ] DropGraph updates in real time during an active build, tracking actual DSP state (not a simulated/GUI-only animation)
- [ ] Drop point and current transition position clearly visible
- [ ] Active-effects indication reflects actual IntensityBudget weights, not static UI state
- [ ] No CPU spikes from visualization (profile confirms UI update cost is negligible relative to DSP)
- [ ] UI represents processor state only — never owns/originates state (verified: closing and reopening the editor mid-transition shows correct current state, not reset state)

---

## Implementation Flow

- Stage 1: Foundation - project structure (JUCE version/CMake/format inspection, skeleton per implementation-spec.md Phase 1)
- Stage 2: Shell - full 19-parameter APVTS + non-parameter ValueTree state scaffolding
- Stage 3: DSP - 5 phases
  - Phase 3.1: Timing & Control Skeleton
  - Phase 3.2: Core DSP Modules
  - Phase 3.3: Creative DSP Modules
  - Phase 3.4: ImpactEngine
  - Phase 3.5: IntensityBudget / DropStyle / AudioAnalyzer / Presets
- Stage 3: GUI - 3 phases
  - Phase 4.1: Layout and Basic Controls
  - Phase 4.2: Parameter Binding and Interaction
  - Phase 4.3: Visualization (DropGraph)
- Stage 3: Validation - 16-preset library completion, pluginval, full DAW-testing checklist (transport start/stop, looping, tempo changes, sample-rate changes, mono/stereo, variable block sizes, offline rendering), profiling, changelog

---

## Implementation Notes

### Thread Safety
- All parameter reads via atomic APVTS accessors; Style index and pending Chaos seed via `std::atomic`.
- No locks anywhere in the audio thread; no background thread needed (no file I/O in this plugin).
- Audio→GUI visualization via lock-free SPSC ring buffer, drained by message-thread `juce::Timer`.

### Performance
- Estimated total ~20-30% single core at 48kHz stereo under the "always-process, weight-mix" architecture (see architecture.md > Special Considerations > Performance for the per-component breakdown).
- Highest-variance components to profile first: PitchModule (grain engine) and SaturationModule (selective oversampling).

### Latency
- Target zero reported latency (`getLatencySamples() == 0`). PitchModule's granular approach is inherently near-zero-added-latency (1:1 sample production, not block-buffered). Revisit `DryWetMixer` latency-compensation configuration only if a future module requires reporting non-zero latency.

### Denormal Protection
- `juce::ScopedNoDenormals` at the top of `processBlock()`.
- Custom accumulator-style state (PitchModule grains, envelope ramps, AudioAnalyzer correlation) explicitly reset during confirmed-silence windows rather than allowed to decay toward subnormal values.

### Known Challenges
- **PitchModule artifact risk (HIGH):** isolate and unit-test before integration; fallback path (reduced semitone set, then delay-line-based shifter) documented in architecture.md if grain-based quality is unacceptable.
- **Sample-accurate segmentation correctness (HIGH, cross-cutting):** centralize the segmentation loop in one place rather than per-module, and build the dedicated multi-block-size unit test harness described in Phase 3.1 before writing any module that depends on it.
- **Mid-build tempo change behavior:** architecture.md latches tempo/duration targets at Arming rather than continuously re-deriving from live tempo — flagged as a decision worth revisiting post-testing if it feels wrong, isolated entirely inside MusicalClock/DropStateMachine.
- **IntensityBudget tuning (MEDIUM, subjective):** the proportional-cap normalization is a testable invariant but the *musical feel* of the result requires actual listening iteration once Phase 3.2's placeholder budget is replaced by the real one in Phase 3.5 — budget dedicated listening/tuning time here, not just correctness-testing time.
- **Reference:** `plugins/KICKR/.ideas/improvements/pitch-module-and-filter-slope-research.md` and `plugins/KICKR/Source/DSP/PitchEnvelope.h` — prior in-repo pitch-related research to review before Phase 3.3, despite KICKR's pitch work targeting a synthesized oscillator rather than program-audio pitch shifting.
- **juce8-critical-patterns.md compliance checklist for GUI phases:** `juce_generate_juce_header()` placement, `NEEDS_WEB_BROWSER TRUE`, explicit resource-provider URL mapping, `check_native_interop.js` inclusion, `WebSliderParameterAttachment` 3-argument constructor, `getToggleState()` for the boolean `trigger`-adjacent UI elements, automation gesture bracketing on every control, WebView cache-busting during active development.

---

## References

- Creative brief: `plugins/DROP/.ideas/creative-brief.md`
- Full implementation spec (authoritative): `plugins/DROP/.ideas/implementation-spec.md`
- Parameter spec (draft — final spec pending mockup, with corrections noted in architecture.md's Resolved Open Questions): `plugins/DROP/.ideas/parameter-spec-draft.md`
- DSP architecture: `plugins/DROP/.ideas/architecture.md`
- UI mockup: not yet created (deferred, parallel workstream per creative-brief.md Next Steps)

**Reference plugins in this repo:**
- **Transitionist** (`plugins/Transitionist/`) — sibling transition-chain plugin explicitly named in creative-brief.md's Inspirations; architecture.md documents how DROP differs (coordinated IntensityBudget staging vs. Transitionist's simpler chain) — review its `PluginProcessor`/`PluginEditor` for repo conventions (build system, WebView setup patterns) during Stage 1 Foundation, though its DSP approach is not directly reusable given DROP's much greater DSP scope.
- **KICKR** (`plugins/KICKR/`) — has prior pitch-envelope and filter-slope research/implementation (`Source/DSP/PitchEnvelope.h`, `.ideas/improvements/pitch-module-and-filter-slope-research.md`) worth reviewing before Phase 3.3, despite targeting a synthesized kick oscillator rather than arbitrary program-audio pitch shifting.
