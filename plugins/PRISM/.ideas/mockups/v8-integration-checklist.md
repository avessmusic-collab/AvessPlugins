# Stage 3 (GUI) Integration Checklist - PRISM v8

**Plugin:** PRISM
**Mockup version:** v8 (finalized 2026-10-15; full restyle of v7, parameter contract unchanged)
**Files:** `v8-ui.html`, `v8-PluginEditor.h`, `v8-PluginEditor.cpp`, `v8-CMakeLists.txt`, `../parameter-spec.md`

Bindings are **event-based** (`prismParam`, `prismUiState`, `prismReady`, `prismParamSync`, `prismInit`, `prismFrame`), not WebSliderRelay-based. There are no relays or parameter attachments.

## 0. Preconditions (Stage 1 / Stage 2 must provide)

- [ ] `parameter-spec.md` is present and the APVTS layout matches it exactly: 17 parameters, ids verbatim, choice strings verbatim (`"1024"`, `"-90"`, `"hann"`, `"blackmanHarris"`, `"leftRight"`, `"Coral/Mint"`, ...).
- [ ] `PRISMAudioProcessor` exposes `juce::AudioProcessorValueTreeState apvts` (or rename `processorRef.apvts` in the editor).
- [ ] `getStateInformation` / `setStateInformation` serialize `apvts.state` (this also persists `src1_name`, `src2_name`, `ui_row_splits`, `ui_lower_col_splits`, `ui_fullscreen`, `ui_spectrogram_orientation`, `ui_width`, `ui_height`).
- [ ] Processor publishes immutable analysis snapshots (`prism::AnalysisSnapshot`, `std::shared_ptr<const ...>`) from a worker thread. Nothing in `processBlock()` touches the editor or the WebView.

## 1. Copy UI files

v8 layout recap: edge-to-edge black screen, five flush 1px-bordered panes (the shared borders are the drag dividers), right sidebar (PRISM nameplate, Loudness / True Peak / Stereo cells, needle meter for momentary LUFS, 4 knobs, 6 buttons), footer rail. Default splits: rows 226 / 70 / 302, lower columns 296 / 266 / 272.

- [ ] Create `plugins/PRISM/Source/ui/public/`.
- [ ] Copy `v8-ui.html` to `Source/ui/public/index.html`.
- [ ] No other files needed (no `js/juce/index.js`, no CSS/fonts; page is self-contained, system monospace font stack only).

## 2. Editor files

- [ ] Move the `prism::AnalysisSnapshot` block out of the editor header into `Source/Analysis/AnalysisSnapshot.h` (keep the `PRISM_ANALYSIS_SNAPSHOT_DEFINED` guard) and include it from `PluginProcessor.h`.
- [ ] Copy `v8-PluginEditor.h` to `Source/PluginEditor.h` and `v8-PluginEditor.cpp` to `Source/PluginEditor.cpp`; adapt class names if the processor is not `PRISMAudioProcessor`.
- [ ] Verify member order in the header: `webView` is declared before `paramBridge`, and the Timer is stopped first in the destructor.
- [ ] Verify the parameter table (`kParams`) lists the 17 ids exactly as in `parameter-spec.md`, in spec order.
- [ ] Wire the Stage 2 seams (currently stubs, UI idles cleanly without them):
  - [ ] `fetchLatestSnapshot()` -> `processorRef.getLatestAnalysisSnapshot()` (atomic shared_ptr load).
  - [ ] `requestResetAll()` -> clear integrated loudness, LRA, max true/sample peaks.
  - [ ] `requestResetIntegrated()` -> integrated LUFS + LRA only.
- [ ] Optional: set `snapshot.bpm` (NaN -> dashes in the waveform header).
- [ ] Fill the snapshot contract: spectrum 640 log-spaced dBFS points per source; waveform columns (bass, mids, highs, flag) at ~84.33 columns/s; goniometer L/R pairs; scope window >= 20 ms + 800 samples at the host rate (kScopeSamples = 8192 covers up to 192 kHz); loudness M/S/I/LRA; max TP/SP since reset; correlation/width/balance. Unavailable values = NaN (sent as null, rendered as dashes).
- [ ] Preset handling: do NOT apply preset bundles in C++ (JS fans them out as individual `prismParam` posts).

## 3. CMakeLists.txt

- [ ] Merge `v8-CMakeLists.txt`: `NEEDS_WEB_BROWSER TRUE`, `NEEDS_WEBVIEW2 TRUE` in `juce_add_plugin`.
- [ ] `juce_add_binary_data(PRISM_UIResources SOURCES Source/ui/public/index.html)` and link it.
- [ ] `juce::juce_gui_extra` linked; `JUCE_WEB_BROWSER=1`, `JUCE_USE_CURL=0` defined.
- [ ] Windows: `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`.
- [ ] Re-run CMake configure after adding the binary data target.

## 4. Build and test (Debug)

- [ ] Build succeeds without warnings (VST3, AU, Standalone).
- [ ] Standalone shows the full UI (not blank); right-click context menu is suppressed.
- [ ] Safari Web Inspector (macOS) shows no JS errors; `window.__JUCE__` exists; `/prism-init.js` loads (200).
- [ ] With the stubbed snapshot source: footer flag shows "NO DATA - waiting for audio", spectrum shows "No data - waiting for audio", all readouts show dashes, nothing animates from fake data.
- [ ] After Stage 2 data flows: flag turns "LIVE", spectrum/spectrogram/waveform/vectorscope/scope/readouts respond; silence -> "NO SIGNAL"; stopping the editor timer -> back to "NO DATA" within 1 s.

## 5. Build and test (Release)

- [ ] Release build succeeds.
- [ ] Open/close the editor 10 times in a DAW: no crash, no leaked timer (destruction order: paramBridge -> webView).
- [ ] Editor open with no audio running does not spike CPU (60 Hz timer is cheap when no new snapshot sequence).

## 6. Parameter binding (all 17 + 2 state)

- [ ] Each Settings popover control and each sidebar knob/button changes the matching APVTS parameter (check with a host parameter list / automation lane): fft_size, window, smoothing, spectrum_tilt, display_range, spectrogram_speed, peak_decay, freeze, analysis_mode, color_theme, preset, reduced_motion, glow_intensity.
- [ ] Footer chips, legend and Sources popover all drive `src1_visible` / `src2_visible`; the S tags and Solo buttons drive `src1_solo` / `src2_solo`.
- [ ] Host automation of any parameter updates the UI (via `prismParamSync`) and does not echo a write back.
- [ ] Preset recall in the host restores all values; the editor opens with correct values on first paint (seeded by `/prism-init.js`, then reconciled by `prismInit`).
- [ ] `src1_name` / `src2_name` rename updates chip, legend, selectors and readout tags; max 10 chars; persists across plugin reload.
- [ ] Choice values: `fft_size` and `display_range` arrive as numbers and map to the right choice index; `Coral/Mint` etc. map by string.
- [ ] RESET clears integrated LUFS, LRA, max peaks; RESET I clears integrated LUFS + LRA only.
- [ ] Spacebar toggles `freeze` (not while typing in a name field); `F` toggles fullscreen; `Esc` exits.

- [ ] Sidebar knobs: SMTH -> `smoothing`, TILT -> `spectrum_tilt`, GLOW -> `glow_intensity`, SPD -> `spectrogram_speed` (vertical drag, wheel, double-click resets to default); knob value labels follow host automation.
- [ ] Sidebar buttons: FREEZE toggles `freeze`; RESET / RESET I fire `reset_all` / `reset_integrated`; THEME shows the current `color_theme` and cycles through all 7; SOURCES / SETTINGS open the popovers.
- [ ] Needle meter tracks momentary LUFS from the snapshot, rests at -40 with no data, and shows `---.--` when unavailable.
- [ ] Corner annotations (FFT/WIN/RES, SMTH/TILT/RNG, BPM/SPAN, SPD, CORR) show live values; BPM shows dashes unless the snapshot supplies `bpm`.

## 7. Serialized UI state

- [ ] Drag each of the 4 pane borders (hairline brightens, grip appears); reload the plugin/project: splits restored (`ui_row_splits`, `ui_lower_col_splits`).
- [ ] Double-click a divider resets that axis.
- [ ] Fullscreen toggle persists (`ui_fullscreen`) and restores on reload; it fills the editor bounds (not OS fullscreen).
- [ ] Resize the editor: aspect locked 55:36, min 770x504, size persists (`ui_width`, `ui_height`).

- [ ] Spectrogram orientation toggle (V/H, right side of the spectrogram pane header): V = vertical waterfall (default); H = horizontal scroll (frequency bottom to top, time right to left). Switching reprojects the existing history (nothing is cleared) and swaps the axis labels.
- [ ] Orientation persists: `ui_spectrogram_orientation` ("v"|"h") is saved in `apvts.state`, sent in `prismUiState`, and seeded via `/prism-init.js` (`__PRISM_INIT_UI.spectrogram_orientation`); reload the project and the choice is restored. It is UI state, not a parameter (parameter count stays 17).
- [ ] Spectrogram pane minimum width is 224 px (dragging the spectrogram|vectorscope border stops there so the header never overlaps); vectorscope and scope keep 180 px.
- [ ] The `20k` label on the spectrogram frequency axis is hidden when the pane is narrower than 270 px.

- [ ] Gestures: dragging a knob/slider shows ONE undo/automation-write gesture in the host (begin on press, end on release); mouse wheel is one begin/value/end burst; no unbalanced gesture after closing the editor mid-drag.
- [ ] Hide the editor/tab, change parameters via host automation, show it again: the UI shows the current values (full `prismInit` is re-pushed on regained visibility).
- [ ] Load a preset / project state while the editor is open (`setStateInformation`): params, source names, splits, orientation, fullscreen and window size all update in the open editor.
- [ ] Out-of-range saved splits are clamped on restore (C++ and JS) to the documented minimums and fixed totals (rows 598, cols 834).
- [ ] Vectorscope: a hard-left test signal traces the upper-left (L) diagonal; the spectrogram does not change when `spectrum_tilt` changes.

## 8. WebView-specific validation

- [ ] No viewport units in CSS/JS (`vh`, `vw`, `dvh`, `svh`); `html, body { height: 100% }` present.
- [ ] `user-select: none` on the body (text inputs re-enable selection).
- [ ] Resource provider serves `/`, `/index.html` (text/html) and `/prism-init.js` (application/javascript); query strings are stripped.
- [ ] Retina: canvases render at devicePixelRatio (<= 2) and remain sharp.
- [ ] Windows: WebView2 runtime present, user data folder writable.

## 9. Parameter list (from parameter-spec.md)

| id | Type | Default |
|----|------|---------|
| fft_size | Choice 1024/2048/4096/8192/16384 | 4096 |
| window | Choice hann/blackmanHarris | hann |
| smoothing | Float 0-100 % | 35 |
| spectrum_tilt | Float 0-4.5 dB/oct | 0 |
| display_range | Choice -48/-60/-72/-90 | -90 |
| spectrogram_speed | Float 0.25-4 x (log) | 1.0 |
| peak_decay | Float 3-60 dB/s | 12 |
| freeze | Bool | false |
| analysis_mode | Choice leftRight/midSide/combined | combined |
| color_theme | Choice (7) | Coral/Mint |
| preset | Choice (6) | Studio |
| reduced_motion | Bool | false |
| glow_intensity | Float 0-100 % | 55 |
| src1_visible | Bool | true |
| src1_solo | Bool | false |
| src2_visible | Bool | true |
| src2_solo | Bool | false |

State entries: `src1_name` ("Ch1"), `src2_name` ("Ch2"). Events: `reset_all`, `reset_integrated`, `prismUiState`.

---

## 10. Manual test checklist (user) - Stage 3 complete, remaining human-only items

Automated / harness-verified in Stage 3 (no action needed): idle boot, live data flow (debug tone: 1 kHz peak, LUFS/TP/corr values), reset_integrated clears I + LRA only, freeze/parameter round trip via the page bridge, sanitized restore of corrupt saved state, multi-instance isolation (two processors + editors in one process), host-chunk project reload including recall while the editor is open, editor open/close lifecycle, 463 console checks.

Do these in a real host (build with the default OFF debug flags; the installed VST3/AU already are):

**A. DAW matrix** - for each: Ableton Live 12 (VST3 and AU), Logic Pro (AU), Reaper (VST3), plus the Standalone.
- [ ] Plugin scans, instantiates, editor opens with the full UI (not blank) on first open.
- [ ] Audio flows through unchanged (null test vs bypass: bit-identical).
- [ ] Footer shows LIVE with audio, NO SIGNAL on silence, NO DATA shortly after the editor tab is hidden.
- [ ] Sidechain input (Ch2) lights up when a sidechain is routed; spectrum/waveform/scope/vectorscope show both sources.
- [ ] Close and reopen the editor 10 times: no crash, CPU does not creep, no zombie window.
- [ ] Two PRISM instances in one project: different theme/splits/names stay independent after save + reload.

**B. Parameters and automation**
- [ ] Turn each of SMTH / TILT / GLOW / SPD (drag, wheel, double-click reset): the host's parameter list moves; one undo step per drag.
- [ ] Record automation on freeze, color_theme, smoothing: playback moves the UI controls; no jitter/echo while writing.
- [ ] Save project, reload, confirm theme, names, splits, orientation, fullscreen, window size all restored.
- [ ] Host preset recall while the editor is open updates the UI; choosing a PRISM preset in the page sets the bundle once, a host-driven preset change does not re-apply it.

**C. Interaction**
- [ ] Space toggles freeze, F toggles fullscreen, Esc leaves it; none of them fire while typing in a source-name field.
- [ ] RESET clears integrated LUFS, LRA and max peaks; RESET I clears integrated LUFS + LRA only.
- [ ] Drag all four pane dividers, double-click to reset an axis; resize the window (aspect locked 55:36, min 770x504).
- [ ] Retina and non-Retina displays: text and canvases are sharp; move the window between displays.

**D. Soak / performance (in the DAW)**
- [ ] 1-hour soak with playback: Activity Monitor shows flat memory for the host and the WebContent process.
- [ ] CPU of the WebKit GPU/WebContent helper processes stays low at 60 Hz (gate: WebView < 25% of one core; if exceeded, enable the gonio/scope 30 Hz decimation hook, kDecimateGonioScopeTo30Hz in PluginEditor.cpp - the page already holds the last arrays).
- [ ] Windows only: WebView2 runtime present, user-data folder writable.
