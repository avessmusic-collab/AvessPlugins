# PRISM - Implementation Plan

**Date:** 2026-10-15
**Complexity Score:** 5.0 (Complex; raw 2.0 + 12 + 2 = 16.0, capped at 5.0)
**Strategy:** Phase-based implementation (Stage 2 in 7 phases, Stage 3 in 4 phases)
**Architecture:** `plugins/PRISM/.ideas/architecture.md` (contract). Parameters: `parameter-spec.md` (LOCKED).

---

## Complexity Factors

- **Parameters:** 17 APVTS parameters (17/5 = 3.4, capped at 2.0) = **2.0** (+2 ValueTree state entries, 3 bridge events, 8 serialized UI-state properties; not counted)
- **Algorithms:** 12 DSP/analysis components = **12**
  1. Capture ring + audio-thread peak/beat capture
  2. Analysis worker/scheduler
  3. Stream history
  4. FFT spectrum engine (windows, coherent-gain normalisation, power-mean)
  5. 640-point log mapper
  6. K-weighting (double biquads)
  7. Gated loudness M/S/I (histogram gating)
  8. LRA (TECH 3342)
  9. True-peak 4x polyphase + sample peak
  10. Stereo metrics (corr/width/balance)
  11. Scope + goniometer capture
  12. 3-band waveform extractor (+ beat flags) and snapshot publisher/mailbox (counted as one pipeline stage)
- **Features:** **+2**
  - FFT / frequency-domain processing (+1)
  - Multiband processing - 3-band LR4 split for the waveform strip (+1)
  - (Not counted: feedback loops - none; modulation systems - none (UI-side smoothing only); external MIDI - none)
- **Total:** 2.0 + 12 + 2 = 16.0 -> **capped at 5.0 (maximum complexity)**

Practical effort is dominated by (a) loudness/true-peak standard conformance, (b) real-time-safe threading, and (c) WebView frame throughput - not by raw algorithm count.

---

## Stages

- Stage 0: Research & Planning (this document + architecture.md)
- Stage 1: Foundation + Shell (CMake, buses, APVTS, state, placeholder editor, contracts compile)
- Stage 2: DSP + Analysis engine (7 phases)
- Stage 3: GUI integration with the existing v8 scaffolding (4 phases)
- Stage 4: Validation (pluginval, DAW matrix, profiling, docs, changelog)

---

## Stage 1: Foundation + Shell

**Goal:** A buildable VST3 + AU + Standalone that has the final bus layout, all 17 parameters, state persistence, and the empty analysis seams, with a placeholder editor (the v8 editor lands in Stage 3 to avoid double work).

**Tasks**
- `CMakeLists.txt` merged from `mockups/v8-CMakeLists.txt`: `NEEDS_WEB_BROWSER TRUE`, `NEEDS_WEBVIEW2 TRUE`, `juce_gui_extra`, `juce_dsp`, `JUCE_WEB_BROWSER=1`, `JUCE_USE_CURL=0`, `juce_generate_juce_header` after link (patterns #1, #3, #9); `IS_SYNTH FALSE`, no MIDI; C++20; separate `PRISM_Tests` console target.
- `Source/Parameters/ParameterIDs.h`, `ParameterLayout.h`: exact 17 ids/choice strings/defaults in spec order, `ParameterID{id,1}`; `spectrogram_speed` custom log `NormalisableRange` (interval 0.01); `fft_size`/`display_range`/`color_theme` choice strings verbatim.
- `PluginProcessor`: constructor `BusesProperties` (main stereo in/out + optional disabled sidechain), `isBusesLayoutSupported` (in==out, mono/stereo; sidechain disabled/mono/stereo), `getBusBuffer` plumbing; `processBlock` **empty but read-only** (no writes); 0 latency; `getTailLengthSeconds()=0`.
- `getStateInformation` / `setStateInformation` with the sanitizer (splits clamp to 598/834 + mins, orientation enum, name <= 10 chars, size aspect/min).
- `Source/Analysis/AnalysisSnapshot.h` (v8 struct moved verbatim), `SnapshotMailbox` (SpinLock-guarded `shared_ptr<const>`), processor API stubs: `getLatestAnalysisSnapshot()`, `requestResetAll()`, `requestResetIntegrated()`, `setEditorActive()`.
- Placeholder editor (blank / `GenericAudioProcessorEditor`).
- Tests: parameter count/ids/defaults vs spec; state round-trip incl. 2 name entries + UI props; sanitizer on corrupt input; bus-layout matrix; bit-exact passthrough smoke test.

**Exit criteria**
- [ ] Builds VST3/AU/Standalone warning-free; loads in Live and in `pluginval` (strictness 5)
- [ ] Parameter list in the host exactly matches `parameter-spec.md` (17, order, defaults, ranges)
- [ ] State round-trips, corrupt state does not crash and is clamped
- [ ] Sidechain bus visible and optional in Live (VST3 + AU)
- [ ] Output is bit-identical to input (test)

---

## Stage 2: DSP + Analysis Engine (phased)

All phases: no allocation/locks/FFT in `processBlock`; analysis core driven by `AnalysisEngine::pump()/buildSnapshot()` so tests run synchronously. Each phase ends with a git commit (orchestrator) and a green `PRISM_Tests`. Test harness rule: **call `setPlayConfigDetails` before `prepareToPlay`** (otherwise `getSampleRate()=0` -> NaN -> silent renders).

### Phase 2.1: Capture, Worker Skeleton, Snapshot Publishing

**Goal:** Audio thread -> ring -> worker -> mailbox -> `getLatestAnalysisSnapshot()` end to end with sequence numbers; nothing measured yet.

**Components:** `SpscFrameRing`, `BeatEventQueue`, audio-thread sample-peak atomic + playhead capture, `StreamHistory`, `AnalysisWorker` (4 ms / 20 ms poll, editor-active gating, steady 60 Hz publish), `Mailbox`, source registry (2 active sources, `kMaxSources=16` capacity), lifecycle (`prepareToPlay` rebuild only on rate/layout change, `releaseResources` joins the thread).

**Test Criteria**
- [ ] Ring: wraparound, overrun drops whole blocks + counts, producer/consumer stress under TSAN, no torn frames (embedded sequence pattern)
- [ ] `processBlock` allocation hook: **zero** `operator new` calls over 100k blocks (mono/stereo, sidechain on/off, block sizes 1..4096)
- [ ] Passthrough bit-exact (random, NaN, denormal, +-inf, full-scale)
- [ ] Snapshot sequence strictly increasing; mailbox fetch never blocks; destruction while editor holds a snapshot is safe
- [ ] Worker starts/stops cleanly 1000x (prepare/release cycles), including mid-processing

### Phase 2.2: Peaks, Stereo Metrics, Scope + Gonio, Silence Semantics

**Goal:** Everything cheap and exact; first real numbers in the UI.

**Components:** sample peak (lossless atomic fold), `StereoMetrics` (400 ms rolling, double), scope window (length per architecture C9, `analysis_mode` channel), gonio (900 pairs, stride 3@48k), `signalPresent`, non-finite sanitation counters, `reset_all` epoch for peaks.

**Test Criteria**
- [ ] Correlation: identical -> +1, inverted -> -1, independent noise -> 0 +- 0.05, hard-left -> 0 (one channel silent -> denominator guard), silence -> 0
- [ ] Width: mono 0, anti-phase 100, hard-panned 50 (+-0.1); balance: R = sqrt2 x L -> +3.01 dB, hard-left -60 clamp, silence 0
- [ ] Sample peak exact for known impulses (incl. peak at block boundary, blocks of 1 sample); lossless when ring overruns
- [ ] Scope length = `min(8192, ceil(0.02 fs)+800+256)` at 44.1/48/96/192 kHz; last-N contiguity verified against a counter ramp
- [ ] Gonio for hard-left/right/mono produces the documented diagonals after the JS mapping (checked in numbers: mid/side)
- [ ] Mono source: `hasStereo=false`; sidechain absent: source 1 fully empty (no fabricated zeros)

### Phase 2.3: Spectrum Engine

**Goal:** Calibrated 640-point dBFS spectrum, all sizes/windows/modes, all sample rates.

**Components:** window tables + `S`, 5 FFT orders, hop N/4 on absolute frame index, power-mean combine, 640-point log map (HF max / LF dB-interp), `analysis_mode` domains, floor -120, warm-up (`hasSpectrum`), size/window switching at hop boundaries.

**Test Criteria (reference signals; per size x window x fs in {44.1, 48, 96, 192 kHz})**
- [ ] **Sine, bin-centred** at -0.1/-20/-60 dBFS: peak reads true level +-0.1 dB; frequency of the peak point within one log-point (0.0156 oct)
- [ ] **Sine, off-bin / worst-case half-bin:** error <= 1.5 dB (Hann), <= 1.0 dB (Blackman-Harris); documented scalloping
- [ ] **White noise** (Gaussian, known RMS): mean per-bin level equals analytical `RMS^2 * (sum w^2)/(sum w)^2 * 2`-based density within +-0.5 dB averaged over 200 frames; Hann vs BH differ only by their ENBW
- [ ] **Impulse:** flat spectrum at `-20log10(S/ (2 w[n0]))`-consistent level (windowed impulse magnitude), no spurious tones
- [ ] **Multitone** (e.g. 100 Hz, 1 kHz, 5 kHz, 10 kHz at -12/-18/-24/-30 dB): each tone's level +-0.1 dB (bin-centred set) and no inter-tone leakage above the window's sidelobe spec (Hann -31 dB first sidelobe, BH -92 dB)
- [ ] Coherent-gain: Hann and BH give the same peak level for the same sine
- [ ] `analysis_mode`: L-only sine -> LR shows it, MS shows -3.01 dB relative (M=(L+R)/sqrt2), combined shows -3.01 dB (power mean); anti-phase sine: combined unaffected (no cancellation), midSide Mid = -inf/-120
- [ ] Silence -> exactly -120 floor (no -inf/NaN); `hasSpectrum` false until N frames are in
- [ ] Hop timing/ block-size independence: rendering the same stream with block sizes {1, 7, 64, 480, 4096} produces bit-identical spectra
- [ ] CPU budget: N=16384 + hop at 192 kHz 2 sources < 5% of one core (release)

### Phase 2.4: Loudness (BS.1770-5 / R128 / TECH 3342)

**Goal:** Standards-compliant M/S/I/LRA.

**Components:** `KWeighting` (analytical coefficients per fs, double), 10 ms chunk sums, momentary/short-term, gating blocks (400 ms/75%), fine histogram integrated, LRA histogram, reset epochs, gap handling, source-0 publish.

**Test Criteria**
- [ ] Coefficients at 48 kHz match the BS.1770 table to 1e-9; stage frequency responses at 1 kHz within +-0.01 dB of the standard at 44.1/48/96/192 kHz (computed analytically)
- [ ] 997 Hz stereo sine at -23 dBFS peak-equivalent per EBU TECH 3341 case 1 reads the reference LUFS within +-0.1 LU for M, S and I; mono-on-one-channel reads 3.01 dB lower than the same on both channels
- [ ] **EBU TECH 3341 / 3342 compliance set** (cases for M, S, I, gating, LRA; the standard's own tolerances: +-0.1 LU I/M/S, +-1 LU LRA): pass all supplied cases at 48 kHz, repeated at 44.1 and 96 kHz via resampled variants
- [ ] **Golden values** generated offline with libebur128 and pyloudnorm for 12 synthetic signals (stepped levels, noise, music-like multitone with silence gaps) committed under `Tests/fixtures/loudness_golden.json`; engine within +-0.05 LU at 44.1/48/96/192 kHz
- [ ] Absolute gate: 30 s at -80 LUFS then 30 s at -23 -> integrated == -23 (+-0.05); relative gate: loud/quiet segment test matches hand calculation
- [ ] `reset_integrated` clears I + LRA only; `reset_all` clears I, LRA, TP/SP maxima; M and S unaffected; NaN after reset until the first gated block
- [ ] Momentary available after 400 ms, short-term after 3 s, integrated after the first gated block (NaN before - never 0)
- [ ] Multi-hour memory: 10 hours simulated -> constant memory (histogram), < 1 ms per publish for the O(8000) integrated recompute
- [ ] **Decision gate:** if golden/EBU tolerances cannot be met after one debugging pass, switch `LoudnessMeter` to vendored libebur128 behind the same interface (documented fallback, R5)

### Phase 2.5: True Peak

**Goal:** Annex 2 true peak, 4x at all rates, with the TP >= SP invariant.

**Components:** polyphase 4x FIR (4 x 12 taps/channel), streaming max, invariant fold, reset.

**Test Criteria**
- [ ] fs/4 sine at 45 deg, amplitude 1.0: SP = -3.01 dBFS, TP within +0.2/-0.4 dB of 0.00 dBTP (EBU TECH 3341 tolerance), at 44.1/48 kHz; and at 96/192 kHz equivalent worst-case phase
- [ ] TECH 3341 true-peak cases (inter-sample overs) within tolerance
- [ ] Invariant TP >= SP on 1e6 random blocks, all rates
- [ ] Peak never decays; `reset_all` clears; -INF -> null before any signal
- [ ] CPU at 192 kHz, 2 sources < 1% of a core

### Phase 2.6: Waveform Columns + Beat Flags

**Goal:** 84.33 columns/s per source with the consumer-acknowledged delivery scheme.

**Components:** LR4 200 Hz / 2.5 kHz splits, column accumulator (fractional period), peak -> 0..1 map, beat/bar flag injection from `BeatEventQueue`, column log (4096), ack scheme with the 64-entry sequence->column map, bpm passthrough.

**Test Criteria**
- [ ] Column rate = 84.33 +- 0.01/s measured over 60 s at every supported rate (no drift)
- [ ] Band separation: 60 Hz sine -> bass only (mids/highs < 5% of bass); 1 kHz -> mids; 8 kHz -> highs; pink noise -> all three non-zero; silence -> exact zeros
- [ ] Flags: host-style ppq playhead simulation at 120 bpm 4/4 -> beat flags every 0.5 s, bar flag every 2 s (+-1 column)
- [ ] Ack scheme: simulate an editor that fetches every 3rd snapshot -> no missing or duplicated columns; stalls > 16 columns drop oldest, count reported
- [ ] No host playhead -> flags 0, `bpm` NaN

### Phase 2.7: Integration, Concurrency, Profiling

**Goal:** Whole engine behaves in the real processor under stress.

**Components:** editor-active/inactive modes, full snapshot assembly, reset plumbing through the processor API, rate change/layout change rebuild, parameter reads (`fft_size`, `window`, `analysis_mode`).

**Test Criteria**
- [ ] End-to-end `processBlock` harness (block sizes 1..8192, fs 44.1-192 kHz, mono/stereo/sidechain on/off): snapshot contents match the synchronous reference path bit-for-bit
- [ ] Parameter changes during processing (fft_size/window/analysis_mode every 10 ms for 60 s): no crash, no allocation on the audio thread, spectra valid after each switch
- [ ] Sample-rate/layout change mid-session: analyzers rebuilt, integrated reset (documented), no stale snapshot published
- [ ] TSAN and ASAN/UBSAN clean on the stress test
- [ ] Profiling: worker % CPU at {44.1, 48, 96, 192 kHz} x {1, 2 sources} x {editor open, closed}: open < 3% @48 k, < 8% @192 k; closed < 0.5% @48 k (recorded in NOTES.md); memory < 10 MB
- [ ] `pluginval` strictness 5 with the engine live

---

## Stage 3: GUI Integration (existing v8 scaffolding, phased)

**Inputs:** `mockups/v8-ui.html` -> `Source/ui/public/index.html`; `v8-PluginEditor.h/.cpp` -> `Source/PluginEditor.*`; `v8-integration-checklist.md` is the acceptance list.

### Phase 3.1: Editor, Resources, Idle UI

- Copy files per checklist section 1-3; resource provider (`/`, `/index.html`, `/prism-init.js`; query stripped; cache-buster); `NEEDS_WEB_BROWSER`/`WEBVIEW2` flags; binary data target; member order (webView before bridge/timer).
- Wire seams with stubs returning nullptr -> UI idle ("NO DATA - waiting for audio").
- [ ] Standalone shows the full UI; Safari Web Inspector clean; `window.__JUCE__` present (R12); editor open/close x10 in a DAW with no leaks

### Phase 3.2: Parameter + State Binding

- `prismParam` (17 + 2 names + 2 momentary), gesture protocol, `prismParamSync`/`prismInit`, `prismUiState`, serialized splits/orientation/fullscreen/size, `valueTreeRedirected` resync, hidden-view resync.
- [ ] Checklist sections 6-7 all pass (automation both ways, preset recall, clamped saved splits, balanced gestures, 55:36 aspect lock)
- [ ] `preset` changes do NOT re-apply bundles in C++

### Phase 3.3: Live Data Flow (Stage 2 -> UI)

- `fetchLatestSnapshot()` -> `processor.getLatestAnalysisSnapshot()` (also records ack/fetch time); `requestResetAll/Integrated` -> processor; `setEditorActive` in ctor/dtor + `visibilityChanged`; optional `reduced_motion` 30 Hz push; per-field decimation hooks (gonio/scope at 30 Hz switch) behind a constant.
- [ ] Footer LIVE / NO SIGNAL / NO DATA transitions correct (silence, transport stop, editor timer stop)
- [ ] All modules respond: spectrum, spectrogram (untilted), waveform (gapless), vectorscope convention (hard-left upper-left), scope, loudness/TP/SP/corr/width/balance readouts, needle meter
- [ ] **Throughput gate:** `pushAnalysisFrame` p99 < 2 ms and WebView CPU < 25% of a core at 60 Hz, 48 kHz and 192 kHz on a reference laptop; if missed, apply risk R2 levers in order
- [ ] Values shown in the UI equal the engine's snapshot values (spot-check against the console harness with the same file)

### Phase 3.4: Hardening

- Reset buttons semantics, freeze, fullscreen, multi-instance (two PRISM instances in one project do not share state), project reload, DAW matrix (Live 12 VST3/AU, Logic AU, Reaper VST3, Standalone), HiDPI, adaptive quality.
- [ ] Checklist sections 4, 5, 8 pass; 1-hour soak shows flat memory

---

## Stage 4: Validation

- `pluginval` strictness 5 (VST3 + AU); `auval` for AU
- Re-sign installed bundles (pattern #27) via `scripts/build-and-install.sh`; verify in Ableton scan
- DAW matrix incl. sidechain routing in Live (VST3 + AU)
- Final accuracy report: table of FFT/LUFS/TP/correlation test results appended to `NOTES.md` ("honest documentation of limitations": scalloping, TP under-read bound, sidechain loudness not shown, integrated resets on SR change)
- CHANGELOG, version 1.0.0, presets N/A (parameter `preset` is UI-only)

---

## Implementation Notes

**DSP/analysis approach:** analysis-only plugin; audio path is never written. All measurement code runs on the worker; the audio thread does memcpy + max + playhead read. See architecture.md sections C1-C11.

**Dependencies between phases:** 2.1 -> everything; 2.2/2.3/2.4/2.5/2.6 independent after 2.1 (can parallelize if desired but commit sequentially); 2.7 requires all; Stage 3.3 requires 2.7 (can start 3.1/3.2 in parallel with 2.x since they only need the Stage 1 seams).

**Key risks (full table in architecture.md):**
1. Host sidechain availability -> optional bus, graceful no-data, test matrix (R1)
2. WebView frame throughput -> measure in 3.3, decimation levers, binary-fetch fallback (R2)
3. Worker scheduling / ring overruns -> 1.37 s ring, lossless SP atomic, gap handling (R3)
4. 192 kHz CPU -> budgets and levers; profile in 2.7 (R4)
5. Loudness accuracy -> EBU/libebur128 goldens; libebur128 vendor fallback (R5)

**Contract ambiguities needing product input (architecture.md section 12):** A1 (`analysis_mode` vs single curve per source), A2 (global loudness = source 0), A3 (stereo-metric window = 400 ms), A5/A6 (scope/waveform channel and band mapping), A8 (true-peak oversampling = 4x everywhere).

**Time estimate (agent sessions, not wall-clock):** Stage 1: 1 session; Stage 2: 7 phases (2.1: 1, 2.2: 1, 2.3: 1.5, 2.4: 2, 2.5: 0.5, 2.6: 1, 2.7: 1.5); Stage 3: 4 phases (~3 sessions); Stage 4: 1-2 sessions.

**Factory reminders:** console tests call `setPlayConfigDetails` before `prepareToPlay`; scoped `git commit -- <paths>` only; do not use `juce::dsp::Limiter` anywhere (hidden compressor - irrelevant here but never applied to the audio path); stage KICKR-style, commit PRISM only on user instruction.
