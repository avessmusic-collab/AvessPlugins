# WARPR (working title) - Implementation Plan

**Date:** 2026-11-11
**Complexity Score:** 5.0 (cap) - **Complex** (raw uncapped score ~22; the formula saturates at 5.0, so WARPR is the factory's largest plugin by a wide margin)
**Strategy:** Phase-based implementation (16 phases: 2 foundation/shell, 10 DSP, 4 GUI) with a verify-compile-test gate and a git commit after every phase

---

## Complexity Factors

Formula: `score = min(param_count/5, 2.0) + algorithm_count + feature_count`, capped at 5.0.

- **Parameters:** 135 APVTS parameters (+ non-parameter state: embedded sample, 10 LFO shapes, mod matrix, 2 shaper curves) -> 135/5 = 27 -> capped = **2.0**
- **Algorithms:** **17** DSP/engine components = **17**
  - SampleStore/Loader/Codec, SampleReader (sinc + mip pyramid), PhaseWarp (bend/phase/sync/FM families), AmpWarp (bend/fold/distortion/quantiser families), Adaa/FastMath, ShaperEngine (A/B LUT + antiderivative), GrainEngine (3 modes + scan), ModOsc, FilterEngine (34 types / 9 families), AmpEnvelope, LfoEngine + shape tables + ModMatrix + TransportClock, Voice/Unison/Synth (poly/mono/legato/glide/stealing), HalfbandIIR oversampling, MasterStage, VizAnalyzer, StateIO/Presets, UiBackendImpl
  - Inside those: **60 warp modes + 34 filter types = 94 independently implemented algorithms** (not counted individually; the formula would exceed any cap)
- **Features:** **3** points (formula-visible) - Feedback loops (+1: Self FM, comb/ladder feedback), Modulation systems (+1: 10 LFOs + matrix + ADSR), External MIDI control (+1: poly/mono/legato, bend, pedal, mod wheel)
  - Not scored by the formula but real extra complexity: file I/O + embedded-state binary container, oversampling architecture, granular engine, lock-free UI data feeds, WebView drag-drop, host transport sync
- **Total:** 2.0 + 17 + 3 = **22.0 -> capped at 5.0**

**Honest assessment:** treat WARPR as ~4-6x the effort of the previous largest factory plugins (CHOPLINE/CORRUPTR class). The two dominant technical risks are **CPU** (32 voices x 8 unison x 8x OS cannot be afforded; a voice/oscillator budget is part of the design) and **aliasing quality** across 60 warp modes.

---

## Stages

- Stage 0: Research & Planning ✓ (this document + `architecture.md`)
- Stage 1: Foundation + Shell <- Next (phases F1, F2)
- Stage 2: DSP (phased: D1-D10)
- Stage 3: GUI (phased: G1-G4, interleaved with Stage 2 after D1, D4/D5, D8, D9)
- Stage 4: Validation (presets, pluginval/auval, DAW checklist, changelog) - folded into D9/D10/G4

**Stage naming note (contract ambiguity A11):** the brief calls the work "Stage 1 Planning"; the parameter spec says "Stage 2 = Shell / Stage 3 = GUI"; the template says "Stage 3 DSP/GUI". This plan uses explicit phase IDs (F/D/G) so the orchestrator can map them to whatever numbering it uses.

### Mapping to the brief's original milestone plan (milestone discipline preserved)

| Brief milestone | Content | Phases here |
|---|---|---|
| M1 project + playback + ADSR + basic GUI | CMake, params, sample engine, ADSR, voices, basic WebView | F1, F2, D1, G1 |
| M2 visualization + bending | Viz feeds, warp framework, bending modes | D2 (+ viz feeds), G3a later |
| M3 folds / distortion + oversampling | 10 folds, 19 distortions, ADAA, OS validation | D3 |
| M4 shaper editor | A/B curve LUT engine + editor backend | D4, G2a |
| M5 filters (+ filter ADSR - cut by UI iteration) | 34 filter types, response display | D5a/b/c, G2b |
| M6 FM / sync / remaining warps | Phase & time, sync, FM/PM/AM/RM/Self/Cross | D6 |
| M6+ (added by later brief revisions) | Granular engine, LFO/mod system | D7, D8, G3 |
| M7 presets + tests | State container, presets, full regression, TSAN/ASAN, pluginval | D9 |
| M8 optimisation / release | Benchmarks, governor, SIMD, Windows CI, package | D10, G4 |

**Per-phase gate (applies to every phase, no exceptions):** (1) all targets build clean (VST3, AU, Standalone; no warnings), (2) `WARPR_Tests` phase subset + full regression pass, (3) zero allocations in `processBlock` (alloc hook), (4) no NaN/inf, (5) pluginval at the end of D1, D5, D9, D10, (6) install per pattern #10/#27 before any manual listening, (7) git commit **scoped to `plugins/WARPR`** (never bare `git commit`; other plugins carry uncommitted work).

---

## Complex Implementation (Score >= 3.0)

### Stage 1: Foundation + Shell

#### Phase F1: Foundation (size S)
**Goal:** Buildable skeleton that matches the contracts.
**Components:** `CMakeLists.txt` from `mockups/v9-CMakeLists.txt` (IS_SYNTH, NEEDS_WEB_BROWSER, NEEDS_WEBVIEW2, `juce_gui_extra`, `juce_audio_formats`, `juce_dsp`, JUCE_WEB_BROWSER=1, binary data incl. `check_native_interop.js`); `PluginIdentity.h`; empty `PluginProcessor` (output-only stereo bus, `isBusesLayoutSupported`), editor from the v9 boilerplate guarded by a stub backend; `WARPR_Tests` target (EXCLUDE_FROM_ALL; patterns from `KICKR_Tests`): offline renderer, alloc hook, NaN/DC/discontinuity scanners, FFT alias meter.
**Test Criteria:**
- [ ] VST3 + AU + Standalone build; plugin appears as an instrument; `WARPR_Tests` runs
- [ ] `juce_generate_juce_header`, relay/attachment member order (pattern #11), nullptr 3rd arg (#12) in place
- [ ] JUCE FLAC writer API (`AudioFormatWriterOptions`) confirmed by a 10-line round-trip test
- [ ] Decision recorded: final PLUGIN_CODE/manufacturer code (A21)

#### Phase F2: Shell (size M)
**Goal:** All 135 parameters + state skeleton + cross-thread primitives.
**Components:** `ParamTable.h` (single X-macro table) generating IDs, `ParameterLayout` in exact spec order, `ParamSnapshot`, editor relay macro tables (verified equal to v9 `WARPR_*_PARAMS`); non-automatable grid params via `AudioParameterChoiceAttributes().withAutomatable(false)`; `LockFreeSwap`, `RetireList`; `VizFifo.h` extracted; `UiBackendImpl` stubs (all interface methods exist); `StateIO` container skeleton (APVTS XML + empty blob + `WARPR_UI_STATE` child).
**Test Criteria:**
- [ ] A script parses `parameter-spec.md` and asserts ID, type, range, default, skew, choice lists and order against the APVTS (all 135) and `v9-PluginEditor.h` macros (55 + 80)
- [ ] 115 automatable / 20 non-automatable verified
- [ ] State round trip of all parameters (random values) is exact
- [ ] `LockFreeSwap` + `RetireList` TSAN stress (message thread publishes at 10 kHz, audio thread consumes): no race, no audio-thread free
- [ ] Editor opens with stub backend (blank-safe), no console "unknown to the backend"

---

### Stage 2: DSP Phases

#### Phase D1: Sample engine + playback + ADSR + voices (size XL) - brief M1
**Goal:** Make sound: default wave and loaded samples play polyphonically with correct pitch, loops, ADSR, glide, unison, voice management.
**Components:** `SampleData`/`MipBuilder`/`SampleCodec`/`SampleStore`/loader thread; default single-cycle wave; `SampleReader` (sinc8 + Hermite4 draft, mip blend); `RegionFold` (Forward/Ping-Pong/One-Shot + seam crossfade); `AmpEnvelope`; `WarprSynth/Voice/Sound` (Poly/Mono/Legato, stealing + ghost slots, sustain pedal, bend ranges, glide); unison (detune, pan, gain); OS-native pipeline scaffold at OS 1/2/4/8 with `HalfbandIIR` decimator (identity warp); `MasterStage` (DC block, gain, safety knee, NaN guard); `transpose/fine/root/start/end/loopMode/direction`.
**Test Criteria:**
- [ ] Pitch accuracy <= +-1 cent over MIDI 0-127, file SR 22.05-192 kHz vs host 44.1-192 kHz
- [ ] Mip alias test: 15 kHz sine sample played +1 oct / +3 oct: alias <= -80 dBc
- [ ] Loop seams: forward/ping-pong/one-shot boundaries, |dy| <= 0.01 at seams (single-cycle default wave bit-exact periodic)
- [ ] ADSR: attack/decay/release timing (-60 dB) within 2%, retrigger and early release click-free, voice freed at -90 dB
- [ ] Voice storm (random notes + pedal + bend + all-notes-off, 10 min simulated): no stuck notes, no NaN, voices idle after release
- [ ] Glide time +-2%, unison cents accuracy +-0.5 cent, L/R balance symmetry <= 0.1 dB
- [ ] Decimator (per stage and cascaded): passband <= 0.1 dB to 0.4 fs, stopband <= -90 dB above 0.6 fs, no instability
- [ ] Mono-sample duplicate: L == R exactly; zero allocations; pluginval pass

#### Phase D2: Warp framework + bending + viz feeds (size L) - brief M2
**Goal:** Warp stage plumbing, DiscreteSwitchFade, SeamBridge, ADAA/FastMath foundations, VizAnalyzer.
**Components:** `WarpModeTable`, `PhaseWarp` (H family: 0,1,2,3,7,9,14), `AmpWarp` V/VT families (4,5,6,8,10,11,12,13), warpEnable/warpMix/warpAmount/A/B smoothing, mode-switch crossfade, `VizAnalyzer` + real `WarprVizFifo` pushes (low/high/rms/transient/theta/r/peaks/voices/lfo -1).
**Test Criteria:**
- [ ] Universal algorithm tests (below) pass for all 15 bending modes
- [ ] `a = 0` identity (-80 dB null) for all; mode switching mid-note max step <= 0.1 FS
- [ ] Viz frame sanity: silent -> rms 0 and tseq constant; transient test increments `tseq` once per hit; peaks match measured output

#### Phase D3: Folds + distortion + oversampling validation (size L) - brief M3
**Goal:** Static nonlinear families with ADAA; prove the anti-aliasing strategy.
**Components:** FOLD 15-22,24; DYN 23; CLIP 25-37; RECT 38-40; QNT 41-43; paired `f/F1` kernels, Simpson ADAA, per-mode `minOS` hints.
**Test Criteria:**
- [ ] Universal algorithm tests pass for all 29 modes
- [ ] `F1' == f` to 1e-5 for every closed-form pair (property test)
- [ ] **A1:** ADAA vs naive at 1x: >= 12 dB alias reduction for Hard Clip, Tanh, Linear Fold, Sine Fold (3 kHz, drive 16)
- [ ] **A2:** 2x + ADAA alias level <= naive 8x + 6 dB for the same shapes; else enable fallback R1(a)/(b)
- [ ] Bitcrush/SR Reduction aliasing is present by design (documented in test as expected), Quantization/Bitcrush identity at a=0

#### Phase D4: Shaper (size M) - brief M4
**Goal:** A/B morphable transfer-curve shaper with LUT + antiderivative ADAA and the full editor backend.
**Components:** `ShaperCurve`, builder (Fritsch-Carlson / linear, symmetric/asymmetric), `ShaperPresetLibrary` (9), `ShaperEngine` (morph, 10 ms LUT crossfade), `UiBackendImpl` shaper methods (set/get/reset/normalize, revision rules, preset-parameter listener).
**Test Criteria:**
- [ ] Identity curve null test; symmetric mode is exactly odd; asymmetric independent halves
- [ ] LUT vs direct evaluation error < 1e-4; Hermite F1 reconstruction error < 1e-6; monotone cubic never overshoots
- [ ] Morph is linear in `f` (A/B blend); NORMALIZE max|y| = 1; RESET identity; JSON round trip canonical
- [ ] Alias test: hard-clip preset ADAA improvement >= 12 dB (A1 criterion)
- [ ] `setShaperCurveJson` does not bump `Revision::shaperCurves` (echo avoidance); preset change does

#### Phase D5a/b/c: Filters (size XL, three sub-phases) - brief M5
- **D5a Standard (types 0-19):** TPT one-pole/SVF cascades, BP, notch/var notch, peak/shelves, key tracking, drive/mix, type-switch crossfade, `FilterResponse` for linear types.
- **D5b Special linear (20-27):** Comb +/- (Lagrange3 delay, loop damping), All Pass, Serial AP, Formant, Vowel, Morph Formant, Phaser.
- **D5c Nonlinear (28-33):** Res LP/HP (ladder), Dist LP, Scream, S&H, Ring Mod (local 2x), small-signal `FilterResponse` for these.
**Test Criteria (each sub-phase):**
- [ ] Analytic response vs measured impulse-response FFT within 0.5 dB (20 Hz - 0.45 fs) for all linear types; documented small-signal limits for nonlinear
- [ ] Stability sweep: cutoff {20, 1k, 20k}, res {0, 0.5, 1}, drive {0, 24}, morph {0, 1}, fs {44.1, 48, 96, 192}: finite, |y| < 8 FS, resonators bounded
- [ ] Random fast-modulation walk (10k parameter changes/s for 10 s): no NaN, no step > bound
- [ ] Type-switch crossfade continuity (all 34 x 34 transitions sampled); key-track octave test; comb pitch +-5 cents; formant peaks within 3% of table; ring-mod carrier line at fc; S&H hold rate = fc
- [ ] `computeFilterResponse` returns 256 points in < 1 ms; `Revision::filterResponse` bumps on any filter param change

#### Phase D6: Phase/time, sync, FM family + mod oscillator (size L) - brief M6
**Components:** PHM 44-50 (Phase Remapping uses shaper curves), SYN 51-53 (BLEP), MOD 54-59, `ModOsc` (BLEP/BLAMP shapes + Sample source), speed-aware mip selection finalised, Self FM/Cross Mod stability limits.
**Test Criteria:**
- [ ] Universal algorithm tests pass for all 16 modes (flagged intentional discontinuities: Hard Sync, Phase Quantization B=0)
- [ ] **A3:** FM/PM/PD/Hard Sync alias at 8x >= 20 dB below 1x
- [ ] Self FM / Cross Mod bounded and NaN-free over the full A/B/a cube at 1x/8x; feedback guard exercised
- [ ] Sample-as-modulator mip correctness (modulator 15 kHz content does not fold); Hard Sync BLEP residual measured
- [ ] Phase Remapping with identity curve = identity

#### Phase D7a/b: Granular engine (size XL) 
- **D7a:** `GrainEngine` CLASSIC + LOOP, window table + normalisation, scan ramp, position/size/shape/var/spread/reverse, preallocated pools, deterministic PRNG, OS upsampler S->OS, playMode/grainMode crossfades, voice playhead reporting.
- **D7b:** CLOUD (<=20 grains/ch), per-grain phase warps (+Self-FM/sync state), oscillator-budget charging by grain cost, unison cap in Granular.
**Test Criteria:**
- [ ] Grain count never exceeds pools (assert + test); zero allocations
- [ ] Size sweeps / position jumps / mode switches: click metric (max |dy| relative to local RMS) <= bound; no step at grain boundaries (windows >= 0.5 ms ramps)
- [ ] Scan: position vs time exact for D, loop/no-loop, SCAN KEY pitch scaling
- [ ] Level normalisation: output RMS within +-1.5 dB across grainSize 2 ms-2 s and density 1-20
- [ ] Deterministic offline renders (bit-identical twice); Classic grain with size = region and no scan approximates Classic playback (spectral null <= -35 dB)
- [ ] CPU benchmark table recorded for Classic/Loop/Cloud x unison x OS

#### Phase D8: LFO system + modulation matrix (size XL)
**Components:** `LfoTableBuilder` (exact preset functions + bends + custom curves + drift/S&H), `LfoEngine` (FREE/RETRIG/ENV/MONO, directions, Hz/Sync tables, phase, smooth, slew), `TransportClock`, `ModMatrix` (compile, destination classes V/G/N, LFO-on-LFO ordering), hook-up of every modulatable parameter, backend (`setLfoCount/Shape/ModMatrix`, `getUiStateJson`), viz LFO phases.
**Test Criteria:**
- [ ] Golden vectors exported from `v9-ui.html` JS (presetFn, bends, custom curve exponent) match C++ tables within 1e-4 for all 13 presets x bends
- [ ] Sync lock: at 40-300 bpm and all 17 divisions, measured LFO period = `beatsPerCycle` within 0.01%; locked to host ppq after jumps/loops/stop-start
- [ ] Mode semantics (A1 assumptions) verified by tests; Retrig/ENV per-voice independence; MONO phrase reset
- [ ] Matrix math: `norm' = clamp(norm + depth*(2v-1), 0, 1)` exact in the normalised (skewed) domain; Int rounding; structural latching; no zipper (max step per chunk bound)
- [ ] 10 LFOs x 16 voices x 3 routes each at 8x OS: CPU delta vs no-mod <= 15% core

#### Phase D9: State, presets, UI backend completion, full regression (size L) - brief M7
**Components:** `StateIO` final (container, FLAC24/float32 codec, migration hooks), `PresetManager` (user dir, step/save/load), `UiBackendImpl` completion (revisions, preset events, settings dialog skeleton), factory-preset infrastructure (empty), regression suite assembly, TSAN + ASAN runs, pluginval strictness >= 5, `auval`.
**Test Criteria:**
- [ ] Round trip: 135 params + UI state + embedded sample (16/24-bit int and float32 sources, mono/stereo) bit-exact; default wave regenerated; host-state size recorded
- [ ] Corrupt/truncated/garbage state fuzz (>= 1000 mutations): no crash, defaults retained
- [ ] Preset save/load/step with 20 factory-like presets; presetName event
- [ ] TSAN (UI thread hammering setters during audio), ASAN/UBSAN clean; soak 1 h offline
- [ ] pluginval (strictness >= 5) + auval pass; 135-parameter automation list sane (20 hidden)

#### Phase D10: Optimisation, governor, release (size L) - brief M8
**Components:** profiling (Instruments / VTune), SIMD/autovectorisation checks, `Settings` dialog (oscillator budget, interpolation quality, CPU guard), measured CPU table replacing the estimates, memory audit, Windows CI (WebView2 lessons), packaging, CHANGELOG, manual DAW checklist.
**Test Criteria:**
- [ ] Typical patch (8 voices, unison 2, OS 2, warp+shaper+filter, 2 LFO routes) < 15% of one core at 48 kHz / 256 buffer on M-series and 2019 x64; heavy patch documented
- [ ] Budget governor deterministic (unison reduction then steal), never alters sounding voices
- [ ] No regressions vs D9 regression suite; Windows build + pluginval green; AU validated

---

### Stage 3: GUI Phases

#### Phase G1: Layout + basic controls (size M) - after D1
**Components:** copy `v9-ui.html` -> `Source/ui/public/index.html`; JUCE JS files from a working plugin; editor from v9 boilerplate with all 135 relays/attachments (55 macro + 80 LFO), resource provider with explicit URLs + cache-buster, native functions registered; sample load (LOAD + native drop), waveform peaks, ADSR overlay, START/END lines.
**Test Criteria:**
- [ ] Page not blank in Standalone; `window.__JUCE__` present; no "unknown to the backend" warnings (14 native functions, 135 relays)
- [ ] Knobs/selects/toggles move APVTS params and gesture-bracket (pattern #23); relay count == attachment count == 135
- [ ] Sample load works; waveform and playheads show real voice positions
- [ ] Release build: open/close editor 10x in a DAW without crash

#### Phase G2: Parameter binding + interaction (size M) - after D4/D5
**Components:** filter response feed + draggable node sync, shaper editor round trip (A/B, SYM/ASYM, presets, RESET/NORM, morph), warp/shaper tab, grain page swapping, int steppers, envelope handle sync.
**Test Criteria:** [ ] checklist sections 6 items for Sample/Granular/Warp/Shaper/Filter/Global; [ ] host automation reflected in UI; [ ] choice lists populate from APVTS (60 warp, 34 filter counts verified)

#### Phase G3: Advanced UI (size L) - after D8
**Components:** LFO editor (3 default + add to 10), shape presets, tools, per-tab rebind, mod drag-drop + depth rim persisted via `warpr_setModMatrix`, organism/meters/header telemetry from `warprViz`, LFO phase markers, preset bar.
**Test Criteria:** [ ] 10-tab header fit by scoped screenshots (R9); [ ] shapes/matrix persist across editor close/reopen and preset load; [ ] organism perfectly still with silence and reacts with audio; [ ] filter display blank until C++ responds

#### Phase G4: Polish + validation (size M) - with D9/D10
**Components:** native drag-and-drop path, resize 1025x700-2400x1640 uniform scale, Windows WebView2 pass, scoped screenshot verification (never bare full-screen), Dock-hidden click tests, DAW checklist (Ableton/Logic/Reaper).
**Test Criteria:** [ ] checklist sections 5-7 complete; [ ] known WKWebView behaviours handled (pointerup, drops swallowed -> LOAD fallback)

---

### Implementation Flow

- Stage 1: Foundation + Shell - F1, F2
- Stage 2: DSP - D1 -> (G1) -> D2 -> D3 -> D4 -> D5a -> D5b -> D5c -> (G2) -> D6 -> D7a -> D7b -> D8 -> (G3) -> D9 -> D10
- Stage 3: GUI - G1 (after D1), G2 (after D5), G3 (after D8), G4 (with D9/D10)
- Stage 4: Validation - D9/D10/G4 (presets, pluginval/auval, DAW checklist, CHANGELOG)

**Effort estimate (focused agent sessions, not calendar time):** F1 1, F2 2, D1 4, G1 2, D2 3, D3 4, D4 3, D5a 3, D5b 3, D5c 3, G2 2, D6 4, D7a 3, D7b 3, D8 5, G3 4, D9 4, D10 4, G4 2 = **~57 sessions** (roughly 10-14 weeks at one session per working day). Largest risk to the estimate: D3/D6 tuning iterations (aliasing gates) and D7.

---

## Test Strategy (brief section 12, made concrete)

**Harness:** `WARPR_Tests` (console, EXCLUDE_FROM_ALL, KICKR pattern). Two tiers: `--quick` (per-phase gate, < 3 min) and `--full` (nightly/pre-release, cube sweeps). DSP classes tested directly with explicit `prepare(fs, block)`; processor-level tests use `setPlayConfigDetails` **before** `prepareToPlay` (house lesson: otherwise `getSampleRate()==0` -> NaN, silent renders). Alloc hook asserts zero allocations in `processBlock`. Offline renders deterministic (seeded PRNG).

**T1 - Universal per-algorithm checks (every one of 60 warp modes and, adapted, 34 filter types):**
- Parameter cube: warp `a,A,B in {0,0.5,1}` (27 combos; `--quick`: a in {0,1}, A=0.5, B in {0,1}) x OS {1,2,8} x signals {sine 100 Hz, sine 5 kHz, white noise, impulse train, DC 0.5, silence, full-scale square, default wave at notes 24/60/108, a loaded stereo sample} x fs {44.1, 48, 96, 192 kHz} (`--full`).
- **NaN/inf:** none, ever. **Bounded:** voice output |y| <= 4 FS pre-master; master <= 1.0 after safety.
- **DC:** |mean| < 0.01 FS after settle (voice DC blocker) except flagged asymmetric/rectifier modes (<= 0.02 after blocker).
- **Discontinuity:** max |y[n]-y[n-1]| bounded relative to local signal slope for smooth inputs (<= 2 kHz sine); modes with *intended* steps (Flip, Hard Sync, PWM B=0, Wave Skew wrap, Bitcrush, SR Reduction, Quantization, Phase Quantization B=0) carry an explicit expected-discontinuity flag and are checked against their own bound.
- **a = 0 identity:** -80 dB null vs warp-off render (all modes).
- **Smooth parameter sweep:** `a` 0->1 over 100 ms, no zipper (max step bound); **mode switch** mid-note: max step <= 0.1 FS.
- **Determinism:** two identical renders bit-equal. **Zero allocations.**

**T2 - Anti-aliasing gates (measured claims):** sine at 1.1 k / 3.3 k / 8.7 kHz; alias power = energy in bins not at harmonics of f; **A1** ADAA >= 12 dB better than naive at 1x (Hard Clip, Tanh, Linear Fold, Sine Fold, shaper hard-clip preset); **A2** 2x + ADAA <= naive 8x + 6 dB; **A3** FM/PM/PD/Hard Sync: 8x alias >= 20 dB below 1x; **A4** mip source: +3 octave playback alias <= -80 dBc. Failing a gate triggers architecture fallbacks R1.

**T3 - Sample engine:** loop boundaries (forward/ping-pong/one-shot; seam |dy|), pitch accuracy +-1 cent, start/end modulation, reverse, sample swap with sounding voices (no click, no crash, old data freed on message thread), mono vs stereo, 22.05-192 kHz files, tiny (1-sample, 8-sample) and huge (10 min) samples, silence/NaN-containing files scrubbed.

**T4 - Envelope/voices/MIDI:** ADSR timing/retrigger/release, velocity law, note-on sample offset accuracy (<= 1 sample), pitch bend 2/12/24, pedal, mono/legato note stack, glide time, unison detune/spread/gain, stealing fade bound, polyphony 1-32 and live reduction, oscillator-budget behaviour, storm tests.

**T5 - Shaper:** interpolation (smooth monotone / linear), symmetry/oddness, asymmetry, morph, LUT/F1 consistency, preset library bounds, NORMALIZE/RESET/invert-free, ADAA alias gate, JSON canonical round trip, LUT crossfade click bound.

**T6 - Filters:** response (analytic vs measured), stability/NaN sweeps, modulation walk, type-switch crossfade, key tracking, per-type character tests (comb pitch, formant peaks, ring-mod line, S&H rate), drive transparency at 0 dB.

**T7 - Granular:** pool bounds, click metric on size/position/mode changes, scan timing, density, normalisation, determinism, CPU table.

**T8 - Modulation:** LFO golden vectors from the HTML JS, sync lock, modes (A1), matrix math, latching, LFO-on-LFO ordering, transport edge cases (no playhead, stop/start, tempo change, loop).

**T9 - State/persistence/presets:** exact param round trip, UI-state canonical JSON, embedded sample bit-exact (all source formats), truncated/corrupt/garbage fuzz, version migration, preset I/O, 20 simultaneous instances.

**T10 - Oversampling/stereo:** decimator frequency response, OS latched per note (change mid-note: no click, new note uses new factor), stereo symmetry/pan law, mono duplicate exactness.

**T11 - Concurrency/robustness:** TSAN (UI thread hammering all setters + sample loads while audio runs), ASAN/UBSAN, 1 h soak, pluginval >= 5, auval, host sample-rate/buffer-size changes mid-session (prepareToPlay re-entrancy), bypass/offline render determinism.

**T12 - Performance:** benchmark mode `WARPR_Tests --bench`: matrix {voices 8/16/32} x {unison 1/2/4/8} x {OS 1/2/4/8} x {Classic/Loop/Cloud} x {warp families} -> CPU % table committed to `.ideas/` in D10; regression threshold +10%.

---

## Risk Register

| ID | Risk | Likelihood | Impact | Mitigation (built in) | Trigger -> fallback | Phase |
|---|---|---|---|---|---|---|
| R1 | Aliasing in folds/clips/FM/PD/sync at default 2x | High | High | Layered AA (OS + ADAA1 + BLEP + mips), per-mode tests A1-A4 | Gate fails -> ADAA2 / per-mode minOS / default 4x | D3, D6 |
| R2 | CPU: 32 voices x 8 unison x 8x OS (and Cloud x unison) | Certain at extremes | High | Oscillator budget (default 256 units), unison-then-steal, decimator per voice, mono-source halving, stage-wise blocks, opt-in CPU guard, Eco interpolation | Measured typical > 15% core -> Hermite default / lower budget / SIMD | D1 (design), D7, D10 |
| R3 | OS-native pipeline + custom half-band IIR defects | Medium | High | Public JUCE `FilterDesign` coefficients, D1 decimator gate | Fail -> `juce::dsp::Oversampling` Up-trick per voice | D1 |
| R4 | Phase-domain warps on non-periodic samples sound chopped | Medium | Medium | rootNote-defined `Lc`, SeamBridge, periodic default wave, UI hint | Complaints -> optional auto root detection (out of contract, proposal) | D2, D6 |
| R5 | Granular clicks / level pumping / Cloud cost | Medium | High | Windows with >= 0.5 ms ramps, latched params, normalisation LUT, pools, deterministic PRNG, grain-weighted budget | Fail -> Cloud cap, S=1x, per-grain warp degradation (documented) | D7 |
| R6 | 34 filters: ladder/scream stability, comb modulation clicks, formant quality | Medium | Medium | TPT/ZDF + tanh, local 2x, interpolation, crossfaded switches, response tests | Instability -> SVF-with-saturating-integrator fallback | D5 |
| R7 | Embedded-sample state size/speed/API drift | Medium | Medium | Container, FLAC24/float32, size policy, fuzz tests | FLAC writer unusable -> gzip float32 | F1, D9 |
| R8 | WKWebView/WebView2 swallow file drops; Dock eats clicks | Medium | Low | Native `FileDragAndDropTarget` + LOAD button; house lessons | Drops unreliable -> LOAD primary | G4 |
| R9 | 10-LFO tab strip does not fit header | Low-Med | Low | Scoped screenshots at 3/6/10 tabs | Overflow -> compact chips / scrolling strip | G3 |
| R10 | UI vs DSP disagreement (LFO shapes, Fritsch-Carlson, exponent `2^(3c)`) | Medium | Medium | Golden vectors from JS | Mismatch -> C++ authoritative, patch JS | D4, D8 |
| R11 | Voice-management bugs (stuck notes, glide/legato/pedal combinations) | Medium | Medium | Storm tests, ghost slots, note stack | - | D1 |
| R12 | Scope / schedule (94 algorithms, "everything in 1.0" locked) | High | High | Families + generated per-row tests, phase gates, commit per phase, no placeholders | Slip accepted over stubs | all |
| R13 | Cross-thread publication bugs (swap/retire) | Medium | High | Single primitive (`LockFreeSwap`), TSAN from F2 onward | - | F2, D9 |
| R14 | Windows CI/WebView2 pitfalls (FindWebView2, stack, pluginval) | Medium | Low | Apply house lesson file `windows_ci_webview2_lessons` | - | D10, G4 |
| R15 | Rename/ID drift (PLUGIN_CODE, state tags, preset dir) | Low | High | `PluginIdentity.h`, frozen tags, decide code before release, pattern #14 cleanup | - | F1 |
| R16 | Param/relay drift between spec, APVTS, editor | Low | Medium | X-macro single source + spec-parsing test | - | F2 |

---

## Implementation Notes

### Thread Safety
- Audio thread reads parameters via cached atomic pointers once per block; never `getParameter(id)` by string.
- Big objects cross threads only through `LockFreeSwap` + `RetireList` (frees on message thread). `AudioProcessorValueTreeState::Listener` callbacks may arrive on the audio thread: atomics only.
- Viz via SPSC `WarprVizFifo` (drops when full); revisions via atomics.

### Performance
- ~150 flops per oscillator per OS-sample estimate; see architecture R2 table. Budget default 256 OS-units; replace estimates with measurements in D10.
- Process stage-wise over 32-sample chunks (OS 256 samples) for cache and autovectorisation; avoid libm in inner loops (paired fast `f/F1`).

### Latency
- Reported latency 0 (instrument). No look-ahead; decimator group delay not compensated.

### Denormal Protection
- `juce::ScopedNoDenormals` in `processBlock`; DC blockers; feedback structures clamped; envelopes terminate at -90 dB.

### Known Challenges
- Choosing tuned constants for 60 modes (drive ranges, normalisation, A/B roles): use listening sessions + the test cube; record final constants in code next to each mode (spec requirement: "document per mode alongside the DSP").
- JUCE 8.0.15 FLAC writer signature; `AudioParameterChoiceAttributes` availability confirmed locally.
- LFO table exactness vs the JS preview (bends, drift): generate golden vectors early (start of D8).
- Install/sign discipline (patterns #10, #27): always `build-and-install.sh`, then restart DAW; scoped screenshots only for UI verification; scoped `git commit -- plugins/WARPR/...`.
- Rename procedure when the name is chosen: change `PluginIdentity.h`, folder, CMake `PRODUCT_NAME`; **uninstall old bundles first (pattern #14)**; keep `WARPR_UI_STATE`/`WRPR` tags and `PLUGIN_CODE`.

---

## References

- Creative brief: `plugins/WARPR/.ideas/creative-brief.md`
- Parameter spec (LOCKED): `plugins/WARPR/.ideas/parameter-spec.md`
- DSP architecture: `plugins/WARPR/.ideas/architecture.md`
- UI mockup: `plugins/WARPR/.ideas/mockups/v9-ui.yaml`, `v9-ui.html`, `v9-integration-checklist.md`, `v9-PluginEditor.h/.cpp`, `v9-CMakeLists.txt`
- Critical patterns: `troubleshooting/patterns/juce8-critical-patterns.md`

Reference plugins in this repo:
- **KICKR** - `Source/Tests/OfflineRender.*` + `RunTests.cpp` harness, `OversamplingProcessor`, sample library, scoped-commit/versioning discipline
- **CORRUPTR** - `Lfo.h`/`ModMatrix.h`/`ModulationAccumulator.h`, WebView layout and CMake, state-version migration (dead choice entries), pointerup/Dock lessons
- **DrumRoulette** - `juce::Synthesiser` + `AudioFormatManager` usage
- **DROP** - granular lesson (blind OLA self-cancels; position-latched grains here), Limiter hidden-compressor lesson
- **SPECTR/PRISM** - viz/analyzer FIFO patterns, harness lessons
