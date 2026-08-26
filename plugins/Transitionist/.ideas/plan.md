# Transitionist - Implementation Plan

**Date:** 2026-08-26
**Complexity Score:** 5.0 (Complex) — raw total 11.6, capped at 5.0
**Strategy:** Phase-based implementation

---

## Complexity Factors

- **Parameters:** 3 parameters (3/5 = 0.6 points, capped at 2.0) = **0.6**
- **Algorithms:** 9 DSP components (per `architecture.md` Core Components) = **9**
  - Tempo-Synced Delay Line (`juce::dsp::DelayLine`, Lagrange3rd, manual per-sample loop)
  - Feedback Saturator (`std::tanh`, in-loop)
  - Feedback Damping Filter (`juce::dsp::FirstOrderTPTFilter`, in-loop, fixed 8kHz LP)
  - Reverb Engine (`juce::dsp::Reverb` + `freezeMode`-driven freeze/hold)
  - Reverb Modulation (custom LFO-modulated micro-delay, ~0.15Hz / +-3ms)
  - Bipolar DJ Filter (dual `juce::dsp::LadderFilter` instances + dead-zone/smoothstep crossfade)
  - Output Glue (`juce::dsp::WaveShaper` soft-clip + `juce::dsp::Limiter`)
  - Stereo Width (custom M/S encode/decode, wet-only, fixed 1.35x side gain)
  - Dry/Wet Mixer (custom equal-power crossfade)
- **Features:** 2 points
  - Feedback loops (+1): hand-built delay feedback loop with in-loop saturator + filter (explicitly required, not JUCE's built-in feedback-delay abstraction)
  - Modulation systems (+1): reverb tail modulation via a dedicated LFO-modulated micro-delay stage
  - (FFT/frequency-domain: not used — no `juce::dsp::FFT`. Multiband: not used. External MIDI control: not used — pure audio effect.)
- **Raw Total:** 0.6 + 9 + 2 = **11.6**
- **Final Score (capped at 5.0):** **5.0**

**Honest complexity note:** this plugin is genuinely at the top of the complexity range for a 3-parameter plugin in this codebase. The low parameter count (3) is misleading in isolation — the DSP behind each macro is unusually dense: `throw` alone drives 6 distinct internal mappings across 2 components (dry/wet, feedback gain, saturation drive, reverb roomSize/damping/freezeMode/input-mute), `space` drives 2 destinations (tempo-synced division table + reverb roomSize), and `sweep` requires a bespoke dual-filter crossfade architecture with no direct JUCE precedent. This is NOT a "simple 3-knob plugin" despite the parameter count, and should not be understaffed or rushed on that assumption. See `architecture.md`'s "Overall Project Risk" section (MEDIUM-HIGH) for the full risk breakdown.

---

## Stages

- Stage 0: Research ✓
- Stage 1: Planning ✓
- Stage 1: Foundation ← Next
- Stage 2: Shell
- Stage 3: DSP — 3 phases (see below)
- Stage 3: GUI — 3 phases (see below)
- Stage 3: Validation

---

## Complex Implementation (Score = 5.0)

### Stage 3: DSP Phases

#### Phase 4.1: Core Processing

**Goal:** Establish the full 9-stage serial signal chain end-to-end with basic (non-escalating, non-frozen, non-modulated) versions of each stage, so audio audibly and correctly passes through the entire architecture before layering in the plugin's more distinctive/risky behaviors.

**Components:**
- Bus configuration: stereo-in/stereo-out `BusesProperties` (constructor, per `juce8-critical-patterns.md` #4/#12)
- Tempo-Synced Delay Line (Component 1): basic tempo-sync division lookup + host BPM query (with 120 BPM fallback), `SmoothedValue`-ramped delay time, NO feedback loop yet (straight single-tap delay tap for this phase)
- Hand-built feedback loop (Components 2-3): add the `tanh` saturator + `FirstOrderTPTFilter` (fixed 8kHz) INSIDE the delay's feedback path, replacing the straight tap from the previous step; `feedbackGain = 0.85 * throwNorm`
- Reverb Engine (Component 4): basic `juce::dsp::Reverb` with `roomSize`/`damping` driven by `throw`/`space` per the documented formulas — NO `freezeMode` ramp yet (leave at 0 for this phase)
- Bipolar DJ Filter (Component 6): dual `LadderFilter` instances (LPF24/HPF24) with the dead-zone + smoothstep crossfade and cutoff/resonance mapping fully implemented (this is the highest-risk component — get it right early while the rest of the chain is simple to reason about)
- Dry/Wet Mixer (Component 9): equal-power crossfade, `throw`-driven

**Test Criteria:**
- [ ] Plugin loads in DAW/standalone without crashes
- [ ] Dry signal passes through unaffected at `throw=0%`
- [ ] Delay repeats are audible and progressively darken/thicken as `throw` increases (feedback loop working)
- [ ] Tempo-synced division changes audibly and correctly with `space`, tracking host BPM (test at 2+ different host tempos)
- [ ] No click/zipper artifact when `space` changes division during playback (SmoothedValue ramp working)
- [ ] Reverb tail is audible and its size/decay scale with `throw` and `space`
- [ ] `sweep` sweeps a 24dB lowpass (negative) and 24dB highpass (positive) with NO audible click, level jump, or discontinuity when crossing through 0% center — test this explicitly with a slow automation sweep from -100% to +100%
- [ ] Resonance is audibly higher at `sweep` extremes than at center
- [ ] Dry/wet crossfade (`throw`) sounds smooth across its full range with no loudness dip/surge

---

#### Phase 4.2: Parameter Modulation

**Goal:** Add the plugin's "escalating" and performance-gesture behaviors — reverb freeze/hold, saturation drive scaling, and full validation of the joint `throw`/`space` reverb-roomSize interaction — turning the Phase 4.1 skeleton into the actual "throw and hold" instrument described in the creative brief.

**Components:**
- Reverb freeze/hold (Component 4 continued): `freezeMode = smoothstep(throwNorm, 0.90, 1.00)`, `reverbInputGain = 1 - smoothstep(throwNorm, 0.90, 1.00)` explicit input mute, `damping` continuing to approach 0.05 as `throw` approaches 100%
- Feedback saturation drive scaling (Component 2 continued): `drive = 1.0 + 0.6 * throwNorm` (was likely a fixed constant in Phase 4.1 for simplicity — now made `throw`-dependent)
- Full validation of the joint `roomSize = 0.30 + 0.30*throw + 0.40*space` (clamped) formula across the full 2D parameter space of `throw` x `space`

**Test Criteria:**
- [ ] Automating `throw` from 0% to 100% and holding at 100% produces a genuinely sustained/infinite (or near-infinite, several-second-plus) reverb tail — the "throw and hold" gesture
- [ ] Freeze engagement (90%-100% throw) ramps smoothly with no audible click/jump at the 90% threshold or at 100%
- [ ] Backing `throw` off from a frozen state (100% back down through 90%) smoothly un-freezes without an abrupt cutoff
- [ ] Feedback-loop saturation is audibly (subtly) more present at `throw=100%` than at `throw=50%`
- [ ] `roomSize`/`damping` joint formula behaves as documented across at least 4 test points: (throw=0,space=0), (throw=100,space=0), (throw=0,space=100), (throw=100,space=100)
- [ ] No parameter combination produces silence, a hard mute, or an unexpected dropout

---

#### Phase 4.3: Advanced Features

**Goal:** Add the remaining texture/safety stages — reverb modulation, Output Glue, and Stereo Width — and perform the combined high-energy integration test that validates the whole architecture's safety margin.

**Components:**
- Reverb Modulation (Component 5): LFO-modulated micro-delay (~0.15Hz, +-3ms) inserted after the Reverb Engine and before the Bipolar DJ Filter; if this proves difficult to tune cleanly within reasonable effort, fall back to `juce::dsp::Chorus` (very slow rate/low mix) per `architecture.md`'s documented fallback, or drop modulation entirely as a last resort (acceptable scope reduction — see architecture.md Notes)
- Output Glue (Component 7): `tanh(1.15*x)` soft-clip via `juce::dsp::WaveShaper`, followed by `juce::dsp::Limiter` (threshold -1.0dB, release 50ms)
- Stereo Width (Component 8): M/S encode/decode, wet-only, fixed 1.35x side gain
- Final integration/ordering check: confirm strict serial order Delay → Reverb → Reverb Modulation → Bipolar Filter → Output Glue → Width → Dry/Wet Mix exactly matches `architecture.md`'s Processing Chain

**Test Criteria:**
- [ ] Reverb tail has audible slow movement/"aliveness" (or is confirmed dropped as an accepted fallback, with that decision noted in the plugin's changelog/notes)
- [ ] Output Glue is inaudible/transparent at low `throw` (e.g., throw=20%) — no audible clipping character on a moderate input signal
- [ ] Output Glue audibly and effectively contains the plugin's worst-case combined state: `throw=100%` (frozen) + `space=100%` (max division/room) + `sweep` swept to both extremes — no clipping/distortion beyond the intended soft-clip character reaches the output, no hard digital clipping (0dBFS overs)
- [ ] Stereo Width is audible on the wet signal (wider than input) while dry signal (at `throw=0%`) remains exactly as narrow/mono-safe as the input
- [ ] Mono-compatibility spot check: sum output to mono at a high `throw`/`space` setting and confirm no severe phase cancellation/hollowing
- [ ] CPU usage is reasonable (rough target: comparable to the ~25-35% single-core estimate in `architecture.md`'s Performance section) at typical buffer sizes (256-512 samples) and 44.1/48kHz

---

### Stage 3: GUI Phases

**Context:** UI mockup v4 is already finalized (`v4-ui.yaml`) and Stage 3 GUI scaffolding has already been generated during mockup finalization: production `v4-ui.html`, `v4-PluginEditor.h`/`v4-PluginEditor.cpp` templates, a `v4-CMakeLists.txt` snippet, and a `v4-integration-checklist.md` (all in `plugins/Transitionist/.ideas/mockups/`). GUI phases below are lighter-weight than a from-scratch UI build — they are primarily INTEGRATION and VERIFICATION of already-authored scaffolding, not new design work.

#### Phase 5.1: Layout and Basic Controls

**Goal:** Integrate the pre-built v4 scaffolding into the real plugin project structure.

**Components:**
- Copy `v4-ui.html` to `Source/ui/public/index.html`
- Apply `v4-PluginEditor.h`/`v4-PluginEditor.cpp` templates (adapting to match Stage 1/2 Foundation's actual class names/APVTS layout if they differ from the template's assumptions)
- Apply `v4-CMakeLists.txt` snippet's WebView/binary-data resource configuration
- Bind `throw`, `space`, `sweep` via `WebSliderRelay`/`WebSliderParameterAttachment` (3-argument constructor, per `juce8-critical-patterns.md` #12) — note `sweep` is this codebase's first bipolar parameter; verify `apvts.getParameter("sweep")->convertTo0to1(0.0f) == 0.5f` per `parameter-spec.md`'s verification snippet

**Test Criteria:**
- [ ] WebView window opens at 980x420, non-resizable
- [ ] All 3 knob panels visible and styled per mockup (brushed-metal discs, LCD readouts, panel cards, outer frame gradient)
- [ ] `NEEDS_WEB_BROWSER TRUE` set in CMakeLists (per `juce8-critical-patterns.md` #9) — VST3 loads correctly in DAW, not just AU
- [ ] `check_native_interop.js` present and served (per `juce8-critical-patterns.md` #13)

---

#### Phase 5.2: Parameter Binding and Interaction

**Goal:** Full two-way parameter communication, with explicit attention to `sweep`'s bipolar behavior (new territory for this codebase).

**Components:**
- JS→C++ relay calls for all 3 knobs (drag-to-rotate, per `juce8-critical-patterns.md` #16 relative-drag pattern)
- C++→JS updates for host automation (per `juce8-critical-patterns.md` #15 — `valueChangedEvent` callback takes no parameters, must call `getNormalisedValue()` inside the callback)
- `sweep`'s bipolar LCD formatting (signed `-NN%`/`+NN%`/`0%`) and bipolar arc-fill rendering (center-outward in both directions) — logic already implemented in `v4-ui.html`'s `makeBipolarRenderer()` per mockup notes; verify it reads the correct normalized value and produces the correct sign
- Verify `sweep`'s normalized 0.5 maps to displayed "0%" exactly, and normalized 0.0/1.0 map to "-100%"/"+100%"

**Test Criteria:**
- [ ] Dragging each knob updates its parameter and audibly changes the DSP in real time
- [ ] Host automation of all 3 parameters updates the corresponding knob rotation and LCD readout
- [ ] `sweep` at exact center (automate to 0.5 normalized) displays "0%" with zero visible arc fill and the fixed 12-o'clock tick visible
- [ ] `sweep` displays correct sign (`-NN%` vs `+NN%`) on either side of center
- [ ] Preset save/reload restores all 3 parameters correctly, including `sweep`'s bipolar value
- [ ] No lag or visual glitches during rapid automation of any parameter

---

#### Phase 5.3: Advanced UI Elements

**Goal:** Final visual-polish verification pass — this plugin's "advanced" UI elements (bipolar arc-fill, center-detent tick, signed LCD) were already implemented and browser-tested during mockup finalization (`v4-ui-test.html`), so this phase is primarily regression verification inside the actual WebView-hosted plugin (not a from-scratch implementation).

**Components:**
- Bipolar arc-fill rendering (grows outward from 12 o'clock in both directions) — verify inside the real plugin WebView, not just the standalone browser mockup test
- Center-detent fixed tick mark on `sweep`'s track
- LCD ambient glow / brushed-metal knob rendering fidelity vs. mockup screenshots

**Test Criteria:**
- [ ] Bipolar arc-fill renders identically to the `v4-ui-test.html` browser mockup when hosted inside the actual plugin WebView
- [ ] No rendering differences attributable to the WebView environment vs. a standalone browser (font loading, CSS gradients, etc.)
- [ ] Visual polish matches mockup screenshots at 1:1 scale in at least one DAW (test in 2+ DAWs if time allows, per this codebase's general validation practice)

---

### Implementation Flow

- Stage 1: Foundation - project structure, bus config, APVTS with `throw`/`space`/`sweep` (note `sweep`'s bipolar range per `parameter-spec.md`)
- Stage 2: Shell - parameter layout confirmed, empty DSP/GUI stubs building and loading in DAW
- Stage 3: DSP - 3 phases
  - Phase 4.1: Core Processing (full chain skeleton + bipolar filter)
  - Phase 4.2: Parameter Modulation (freeze/hold, saturation scaling)
  - Phase 4.3: Advanced Features (reverb modulation, Output Glue, Width, combined stress test)
- Stage 3: GUI - 3 phases
  - Phase 5.1: Layout and Basic Controls (integrate existing v4 scaffolding)
  - Phase 5.2: Parameter Binding and Interaction (incl. bipolar `sweep` verification)
  - Phase 5.3: Advanced UI Elements (regression verification of already-built bipolar visuals)
- Stage 3: Validation - presets, pluginval, changelog

---

## Implementation Notes

### Thread Safety
- All parameter reads via `apvts.getRawParameterValue(id)->load()`, read ONCE per block into local floats (not per-sample) — critical for the delay's per-sample feedback loop, both to avoid tearing and to avoid atomic-load overhead in a tight inner loop.
- No custom cross-thread communication needed — no file I/O, no background thread, no custom non-APVTS state (see `architecture.md` Special Considerations / Thread Boundaries).

### Performance
- Reverb (~10-15%) and the doubled `LadderFilter` instances (~4-6%) are this plugin's two most CPU-intensive components — profile these first if optimization is ever needed.
- Total estimated ~25-35% single core at 48kHz (see `architecture.md` Performance section) — moderate but not trivial; worth an early CPU sanity check once Phase 4.1's full chain skeleton is in place, before adding Phase 4.2/4.3's additional layers.

### Latency
- 0 samples reported latency — no component in this architecture requires host PDC registration (see `architecture.md` Latency section). Confirm `getLatencySamples()` returns 0 (or is simply never overridden) during Stage 1/Foundation.

### Denormal Protection
- `juce::ScopedNoDenormals` required at the top of `processBlock()` — this plugin has multiple feedback/IIR-style stateful components (delay feedback loop, TPT damping filter, reverb's internal network, both ladder filters), and the reverb's freeze mode in particular is a long-duration low-level-signal state where denormals are especially likely to appear.

### Known Challenges
- **Bipolar filter crossfade (Component 6) is this plugin's highest-risk, most bespoke piece of DSP engineering** — no direct JUCE precedent for a click-free bipolar dead-zone crossfade between two ladder-filter modes; budget real testing time (automated slow sweep from -100% to +100%, listening/measuring for any discontinuity) before considering this component done. See `architecture.md` Implementation Risks for the documented fallback (single-instance `setMode()` switching + brief engineered mute) if the dual-instance approach proves unworkable.
- **Reverb freeze via `freezeMode`** is expected to be lower-risk than the creative brief originally assumed (JUCE ships this exact parameter built-in) — but its precise behavior should be empirically verified in Phase 4.2 before assuming it fully replaces the custom-FDN-reverb idea the brief raised as a possibility. If unsatisfying, escalate per the documented fallback.
- **Reverb Modulation is explicitly the lowest-priority/most droppable component** in this plan — if Phase 4.3 time is tight, it is the first thing to descope (fall back to `juce::dsp::Chorus` or drop entirely) without compromising the plugin's core "throw and hold" identity.
- **`sweep` is this codebase's first bipolar (-100 to +100, default 0 at midpoint) APVTS parameter** — Stage 1/Foundation must get the `NormalisableRange` construction right (see `parameter-spec.md`'s exact code snippet) and verify `convertTo0to1(0.0f) == 0.5f` before moving on; getting this wrong would silently break the entire Bipolar DJ Filter's sign-based routing logic in Stage 3/DSP.
- **Combined `throw` x `space` x `sweep` high-energy state** (frozen reverb + max room + resonant filter extreme) is this plugin's genuine worst-case scenario and should be explicitly tested together, not just parameter-by-parameter in isolation (see `architecture.md` Parameter Interactions and Phase 4.3 test criteria).

---

## References

- Creative brief: `plugins/Transitionist/.ideas/creative-brief.md`
- Parameter spec: `plugins/Transitionist/.ideas/parameter-spec.md`
- DSP architecture: `plugins/Transitionist/.ideas/architecture.md`
- UI mockup: `plugins/Transitionist/.ideas/mockups/v4-ui.yaml`
- UI scaffolding (already generated at mockup finalization): `plugins/Transitionist/.ideas/mockups/v4-ui.html`, `v4-PluginEditor.h`, `v4-PluginEditor.cpp`, `v4-CMakeLists.txt`, `v4-integration-checklist.md`

**Similar plugins for reference (this codebase):**
- TapeAge - reference for `SmoothedValue`-driven parameter ramp patterns and tape/analog-character saturation, if similar techniques were used there
- DriveVerb / FlutterVerb / LushVerb - reference for `juce::dsp::Reverb` integration patterns (modern `prepare`/`process` API, per `juce8-critical-patterns.md` #17)
- AngelGrain - most recent plugin in this codebase with a comparably dense DSP feature set; reference for how a multi-stage wet-path chain with fixed (non-parameter) internal stages was structured and phased
