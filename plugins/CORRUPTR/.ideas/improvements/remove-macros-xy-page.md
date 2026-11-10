# CORRUPTR - Remove Macros / XY Page (Two-Page UI)

**Created:** 2026-11-10
**Type:** Removal
**Aspect:** UI + Parameters + DSP
**Version Impact:** MAJOR (13 parameters removed with state migration) → 3.0.0

## Current State (v2.0.0)

Three pages: Main, Sequencer (+ Mod Matrix), Macros / XY. The Macros / XY page
holds the 8-macro module (MacroEngine fixed routing into ~10 DSP destinations,
macroWidth driving a live M/S width stage) and the XY Pad (assignable axes,
glide, MIDI CC 2/3, Cmd+L latch).

## Change (user request: "remove whole 3rd page, leave only main and modulation pages, remove unnecessary code")

- Delete the Macros / XY page, both modules, and all 13 backing parameters
  (8 `macro*` + 5 `xyPad*`), their DSP routing, MIDI CC handling, and UI code.
- Keep the Mod Matrix "Macro" source choice (dead, reads 0) to avoid
  re-indexing saved Source automation.
- State migration: `corruptrStateVersion` 2→3; strip `macro*`/`xyPad*` PARAM
  entries from older states on load.
- Rename the remaining second tab "Sequencer" → "Modulation".
- Re-voice the 4 factory presets that used macros onto direct parameters.
- Remove the now-identity stereo-width stage and delete `dsp/MacroEngine.h`.

## Testing Criteria

- [x] QA suite: 27 removed params absent; state round-trip; legacy PARAM tolerance
- [x] pluginval strictness 10 — SUCCESS
- [x] Visual check: two pages only (Main + Modulation), CORRUPT/UNDO verified live
