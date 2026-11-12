# WARPR Stage 3 (GUI) Integration Checklist - v9

**Plugin:** WARPR (working title)
**Mockup version:** v9 (finalized 2026-11-11)
**Window:** 1200x820 design size, aspect locked, effective min 1025x700
**Contract:** `.ideas/parameter-spec.md` (135 parameters) - immutable

## 0. Prerequisites
- [ ] Stage 1 plan + Stage 2 shell done: APVTS has all 135 IDs from parameter-spec.md (exact case), LFO grid params registered non-automatable
- [ ] Processor exposes `apvts` and `WarprUiBackend& getUiBackend()` (interface is declared in v9-PluginEditor.h)
- [ ] Read `troubleshooting/patterns/juce8-critical-patterns.md`

## 1. Copy UI files
- [ ] `v9-ui.html` -> `Source/ui/public/index.html`
- [ ] Copy `js/juce/index.js` and `js/juce/check_native_interop.js` verbatim from a working plugin (e.g. `plugins/CORRUPTR/Source/ui/public/js/juce/`) -> `Source/ui/public/js/juce/`
- [ ] Note: the JUCE JS exposes `getToggleState` (not `getToggleButtonState` as the old template says); v9-ui.html already uses the correct API

## 2. Editor + CMake
- [ ] Move `WarprVizFrame` / `WarprVizFifo` (top of v9-PluginEditor.h) to `Source/DSP/VizFifo.h`; include from processor and editor
- [ ] `v9-PluginEditor.h/.cpp` -> `Source/PluginEditor.h/.cpp`; rename classes if the processor uses other names
- [ ] Verify member order in header: relays -> `webView` -> attachments (grep: relay block precedes `webView`, attachments follow)
- [ ] Relay/attachment counts equal: 55 macro params + 10x8 LFO = 135 each
- [ ] Merge `v9-CMakeLists.txt` (NEEDS_WEB_BROWSER, NEEDS_WEBVIEW2, IS_SYNTH, juce_gui_extra, JUCE_WEB_BROWSER=1, binary data incl. check_native_interop.js)
- [ ] BinaryData symbols are file-name based: `index_html`, `index_js`, `check_native_interop_js`
- [ ] All attachments pass the 3rd `nullptr` argument (pattern #12)

## 3. Implement `WarprUiBackend` (Stage 2/3 processor side)
- [ ] `getUiStateJson()` returns `{lfoCount,lfoShapes[],modMatrix[],shaperCurves{A,B}}` per parameter-spec "Non-Parameter State"; same document restored from host state
- [ ] `setLfoCount / setLfoShapeJson / setModMatrixJson / setShaperCurveJson` publish to the audio thread via lock-free swap (no locks/alloc in process)
- [ ] `revision(Revision)` counters: uiState/sample/preset bump on setStateInformation + preset/sample load; shaperCurves bump only for non-UI changes (preset param change, reset, normalize, state load) - never for UI-originated edits (avoids echo mid-drag); filterResponse bumps on any filter param change
- [ ] Shaper preset library lives in C++; selecting `shaperPreset` regenerates the selected curve (editing a point sets preset to Custom)

## 4. Real-data hooks (no fabricated visuals - panels stay empty/relaxed until fed)
Audio thread -> `WarprVizFifo::push()` once per ~33 ms (wait-free, drops if UI behind). Editor timer (30 Hz) pops the newest frame and emits `"warprViz"`.

| Visual | Event / field | Producer (audio or message thread) |
|---|---|---|
| Organism breathing | `low` | LF band envelope follower of post-warp bus |
| Organism shimmer | `high` | HF band envelope follower |
| Organism relax/flow rate | `rms` | overall level follower; 0 = static circles |
| Organism swells | `transient`, `tseq`, `tang` | transient detector; `tseq` increments per hit, `tang` = spectral-centroid angle (rad) |
| Header telemetry theta/r | `theta`, `r` | stereo vector of block energy (atan2(side,mid), length) |
| Master meters | `peakL`, `peakR` | linear peak since last frame, post master gain |
| Waveform playheads | `voices[{pos,level}]` | per sounding voice read position, 0..1 absolute in sample (grain read position in Granular) |
| LFO phase marker | `lfo[0..9]` | phase 0..1 of most recent voice, -1 = idle |
| Waveform peaks + name | `"warprSample"` | `getSampleDisplay()` on `Revision::sample` or `warpr_requestSample` (<=2048 min/max pairs) |
| Filter response curve | `"warprFilterResponse"` `{db[256]}` | `computeFilterResponse()` - real magnitude response of the current filter type/params, log 20 Hz-20 kHz (display range -48..+24 dB) |

- [ ] Frame fields are all sourced from DSP; nothing in v9-ui.html synthesizes them (grep for `Math.random` - only LFO S&H/DRIFT table generation, which is persisted state, not a visual)
- [ ] Verify organism is perfectly still/circular with no audio and reacts with audio
- [ ] Verify filter display is blank (just guide + node) until C++ responds

## 5. Build and test (Debug, then Release)
- [ ] Builds without warnings; install to system per pattern #10 before testing
- [ ] Standalone loads the page (not blank); `window.__JUCE__` exists; console free of "unknown to the backend" warnings (all 14 native functions + 135 relays registered)
- [ ] Release build: open/close editor 10x in a DAW, no crash (member-order check)
- [ ] Windows: WebView2 backend explicit, writable user-data folder (done in PluginEditor.cpp), CI nuget path note in CMake

## 6. Parameter binding tests
- [ ] Every knob/toggle/select/segment moves its APVTS parameter and reflects host automation (spot check one per group; all 63 Choice selects populate from parameter choices - `warpMode` 60 and `filterType` 34 use the UI optgroup lists and log an error if counts mismatch)
- [ ] Knob drag brackets gesture (touch/latch automation records), wheel + double-click reset (defaults = `DEF` table = spec)
- [ ] Int params (rootNote, transpose, polyphony, unisonVoices, grainDensity) step cleanly during drag
- [ ] CLASSIC/GRANULAR swaps rows; grain mode swaps 8th knob (disabled / REV % / DENSITY), SHAPE dims in Cloud
- [ ] Envelope handles on waveform and A/D/S/R knobs stay in sync; START/END lines drag; waveform dims outside range
- [ ] Filter node drag sets cutoff (log) + resonance, stays synced with knobs
- [ ] LFO: tabs 1-3 + add up to 10 (check header fit at 10 tabs by scoped screenshot), mode/dir/sync/grid/rate/phase/smooth rebind per tab, shape presets, point/bend/tools/shift-steps; shapes persist across editor close/reopen and preset load
- [ ] Mod matrix: drag chip onto knob, depth rim/badge drag, dbl-click badge removes; persists in state; LFO rate knob assignments follow the active tab's `lfoNRate` id
- [ ] Shaper: A/B select, symmetric/asym, add/drag/delete points, preset/RESET/NORM round-trip, morph
- [ ] Sample: LOAD chooser, drag-drop wav/aif onto editor (WKWebView may swallow drops - if so, rely on LOAD), sample embeds in state and survives project reload

## 7. WebView validation
- [ ] No viewport units (`grep -E "[0-9]v[hw]|dv[hw]|sv[hw]" index.html` empty) - verified at generation
- [ ] `user-select:none`, context menu disabled, buttons on `pointerup`
- [ ] Resize editor 1025x700 -> 2400x1640: uniform scale, no clipping; canvases stay sharp
- [ ] Scoped screenshots only (window-scoped), Dock hidden for synthetic click tests (macOS Dock eats bottom-80px clicks)

## Parameter list
See `.ideas/parameter-spec.md` (generated from the same table as the macros in v9-PluginEditor.h).
