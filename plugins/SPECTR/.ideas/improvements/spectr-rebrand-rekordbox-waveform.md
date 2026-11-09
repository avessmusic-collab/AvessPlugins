# PRISM - SPECTR Rebrand + Rekordbox-Grade Beat Waveform + Channel-Switched Readouts

**Created:** 2026-11-03
**Type:** Feature + Enhancement (bundled release)
**Aspect:** Identity (rename) / Parameters (theme default) / UI + DSP-adjacent (waveform timeline, readout column)
**Version Impact:** Ships as **SPECTR 1.0.0** (full rebrand; pre-release versioning policy — stays 1.0.0 until public release)

## Current State

- Plugin is named **PRISM** (plugin ID, bundle, installed VST3/AU, folder `plugins/PRISM/`, registry entry, UI nameplate).
- `color_theme` parameter defaults to **Coral/Mint** (theme 1 of 7; Cyan/Violet exists as a non-default choice).
- **Beat waveform strip**: Rekordbox-*style* 3-band mirrored waveform, fixed ~12 s window, playhead right, decorative beat ticks that are not locked to any real musical grid, no zoom, no bar lines/numbers.
- **Right sidebar readouts**: Loudness / True Peak / Stereo cells show per-channel values simultaneously (tinted per source); needle meter shows momentary LUFS of the main input. No way to focus the whole column on one source.

## Proposed Change

### 1. Full rename PRISM → SPECTR
- New plugin ID, bundle ID, VST3/AU identifiers, product name, installed binary names.
- Folder rename `plugins/PRISM/` → `plugins/SPECTR/`; update PLUGINS.md registry, NOTES.md, nameplate in UI, window title, CMake target names.
- **Uninstall PRISM** from system folders when installing SPECTR (clean break; no side-by-side).
- Old DAW test sessions referencing PRISM will show a missing plugin — accepted (not publicly released).

### 2. Default colour theme → Cyan/Violet
- Change `color_theme` parameter default index from Coral/Mint to the existing **Cyan/Violet** theme (blue/purple identity).
- No new theme added; theme count stays 7; all token sets unchanged.

### 3. Rekordbox-grade beat waveform (grid alignment + drawing style + zoom)
Reverse-engineer and match Rekordbox's waveform *presentation*, keeping SPECTR's theme-derived band colours (NOT Rekordbox's blue/amber/white):
- **Beat grid alignment (host transport):** bar/beat lines derived from the DAW's tempo + PPQ position via `AudioPlayHead::getPosition()` (bpm, ppqPosition, timeSigNumerator/Denominator, isPlaying). Downbeat-emphasised bar lines (brighter/taller), lighter beat ticks between, optional bar numbers at bar lines. Waveform pixels rendered sample-accurately against this grid so transients sit exactly on their beats, as in Rekordbox. Grid hidden/dimmed when host provides no position info.
- **Waveform shape & drawing style:** match Rekordbox's half/full mirrored rendering character — per-band peak stacking, their peak scaling/normalisation feel, pixel-column density, playhead fixed-position behaviour with waveform scrolling beneath it.
- **Zoom:** mouse wheel over the strip zooms smoothly between ~4 s and ~60 s visible; double-click resets to default window; zoom level serialized in UI state (alongside row splits/fullscreen).
- **Research task:** detailed study of how Rekordbox renders + aligns (band split frequencies, peak vs RMS column reduction, grid anchoring, zoom steps). Candidate route: deep-research pass before implementation.

### 4. CH1/CH2 selector for the readout column
- Two buttons **CH1 / CH2** heading the whole sidebar readout column (Loudness, True Peak, Stereo cells) — **exclusive toggle**, CH1 default.
- Buttons tinted in each source's theme colour; active one lit.
- The **needle meter follows the selection** (momentary LUFS of the selected source).
- Selection is UI state (serialized), not an APVTS parameter; respects source renames (buttons show current src names if space allows, else CH1/CH2).

## Rationale

- SPECTR is the stronger identity for a spectrum-first analyzer; blue/purple default matches the intended brand look.
- The beat waveform is currently decorative; locking it to the host's real musical grid turns it into a trustworthy phrasing/arrangement tool — consistent with PRISM/SPECTR's "never fabricate values" honesty principle (host PPQ is exact; no fake detected grid).
- DJs/producers already read Rekordbox waveforms fluently; matching its drawing style + zoom makes the strip instantly legible.
- Per-source LUFS/TP focus (CH1/CH2 toggle) makes A/B comparison of main vs sidechain source fast without reading doubled-up cells.

## Implementation Notes

**Rename (touch points):**
- `CMakeLists.txt`: target name, `PRODUCT_NAME`, `PLUGIN_CODE`, `BUNDLE_ID`, company/plugin identifiers.
- Folder `plugins/PRISM/` → `plugins/SPECTR/` (git mv); update any root-build inclusion lists (cf. CHOPLINE root-build exclusion lesson).
- UI: nameplate, document title, any "PRISM" strings in HTML/JS; `prismUiState` bridge key → rename or keep for wire-compat (decide at implementation; renaming is cleaner pre-release).
- PLUGINS.md row, NOTES.md header, CHANGELOG reset/carry-over, `.ideas/` references.
- Uninstall old PRISM binaries from `~/Library/Audio/Plug-Ins/...` on install (plugin-lifecycle skill).

**Theme default:**
- `ParameterLayout.h`: `color_theme` default index → Cyan/Violet. Verify THEME button cycle order and Settings chip row still correct; UI boot state reads default before host state restore.

**Beat waveform:**
- Processor: poll `getPlayHead()->getPosition()` in `processBlock`, push {bpm, ppq, timeSig, isPlaying, ppqOfLastBarStart if available} into the UI bridge alongside existing waveform band data; timestamp waveform columns with their ppq so the UI can anchor grid lines to audio sample-accurately.
- UI (canvas): grid layer drawn from ppq→pixel mapping; bar lines emphasised, beat ticks secondary, bar numbers optional at low zoom.
- Zoom: wheel handler on the strip pane (prevent page scroll), smooth exponential zoom 4–60 s, double-click reset; persist in UI state blob.
- Ring buffer length must cover max zoom (~60 s × 3 bands × columns at max pixel density) — check memory/CPU at 60 fps.

**CH selector:**
- UI-only state + existing per-source metering (LUFS/TP/correlation already computed per source). Needle meter input switches source. Serialize selection in UI state.

**Research (pre-implementation):**
- Rekordbox band crossover frequencies and column reduction method (peak-hold per column vs RMS), half vs full waveform modes, grid anchoring behaviour at tempo changes, zoom step curve. Deep-research or careful visual study with reference screenshots.

## Backward Compatibility

- **Breaking:** plugin ID change — DAW projects using PRISM 1.0.0 will not find SPECTR. Accepted (pre-release; no external users).
- Theme default change only affects fresh instances; saved states keep their stored theme.
- UI-state additions (zoom, CH selection) are additive; absent keys fall back to defaults.
- No APVTS parameter removals; parameter state layout unchanged except the `color_theme` default.

## Testing Criteria

- [ ] SPECTR loads in Ableton Live as VST3 + AU under the new name; PRISM no longer present in plugin lists
- [ ] Fresh instance opens with Cyan/Violet theme; saved sessions keep their chosen theme
- [ ] Bar/beat grid lines align with Ableton's grid: a metronome click / four-on-the-floor kick lands transients exactly on the drawn beat lines at multiple tempi (e.g. 90 / 128 / 174 BPM) and after tempo changes
- [ ] Grid dims/hides gracefully when transport stopped or no position info (standalone)
- [ ] Wheel zoom smooth from ~4 s to ~60 s, double-click resets, zoom persists across editor close/reopen and session reload
- [ ] Waveform drawing style matches Rekordbox reference screenshots (mirrored shape, playhead-fixed scrolling) at comparable zooms
- [ ] CH1/CH2 toggle switches all readout cells + needle meter; tinting correct; selection persists in UI state; renamed sources reflected
- [ ] 60 fps maintained at max zoom with both sources active; pluginval clean; automated test suite passes
- [ ] No regression in spectrum/spectrogram/vectorscope modules

## Success Metrics

- Side-by-side with Rekordbox playing the same track at the same BPM, the strip reads the same: beats on lines, same visual rhythm, comparable zoom feel.
- The rename is total: no "PRISM" string remains in UI, binaries, installer, or registry (grep-clean apart from historical changelog/idea docs).
- Readout column answers "how loud is the sidechain?" in one click.
