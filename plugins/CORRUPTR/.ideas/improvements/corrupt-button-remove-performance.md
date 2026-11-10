# CORRUPTR - CORRUPT Randomizer Button + Performance Section Removal

**Created:** 2026-11-10
**Type:** Feature + Removal
**Aspect:** UI + Parameters + Features
**Version Impact:** MAJOR (parameter removal with state migration) → 2.0.0

## Current State

- CORRUPTR v1.0.2 has a Performance Mode section: 7 MIDI-triggerable live triggers
  (Stutter, Repeat→Glitch, Reverse, Chaos, Destroy, Freeze, Kill), each backed by
  2 APVTS parameters (`performanceX` + `performanceXAmount`) = **14 parameters total**.
- There is no randomization feature; dialing in new sounds means manual tweaking
  across 12 distortion algorithms, 18 glitch modes, the sequencer, and the mod matrix.

## Proposed Change

### 1. Remove the Performance section (full removal + migration)

- Delete the Performance panel from the UI.
- Delete the 7 trigger DSP paths and MIDI-trigger handling.
- Remove all 14 `performance*` parameters from the APVTS layout.
- **Migration:** bump `corruptrStateVersion`; on load of an older state, silently
  drop the `performance*` values so old sessions/presets still load cleanly.
  Breaking only for host automation lanes that targeted those parameters.

### 2. Add a CORRUPT button on the main page

- **Placement:** a new box/panel on the main page, below the glitch section and
  the low-pass filter section.
- **Action:** each click deals a fresh random patch (re-roll).
- **Undo:** one-level undo — restores the exact pre-click state if the result is bad.

**Randomization scope (included):**
- Distortion + bitcrush: algorithm, drive, tone, bias, fold, bit depth,
  sample-rate reduction, distortion mix
- Glitch engine: mode, probability, buffer length, micro-delay, seed
- Sequencer: rate, steps, depth, patterns
- Mod matrix + LFOs + macros: LFO rates/shapes/sync, mod slot amounts, macro values

**Hard constraints (never violated):**
- `feedbackAmount` randomized within **0–10% max** (never above 10%)
- **Filters never randomized**: `filterType`, `filterCutoff`, `filterResonance`,
  `filterMorph`, `filterDrive`, `filterSlope`, `graphBypassFilter` all untouched
- **Levels frozen**: `inputGain`, `outputGain`, `mix`, limiter params
  (`limiterCeiling`, `limiterRelease`, `limiterAutoRelease`, `outputLimiterStyle`)
  and `qualityMode` never touched

## Rationale

The Performance triggers see little use and occupy prime UI space; a one-click
CORRUPT randomizer fits the plugin's "chaos playground" identity (iZotope Trash /
Effectrix lineage) and makes sound discovery instant, while the guardrails
(feedback cap, filter/level freeze) keep every roll safe and mix-usable.

## Implementation Notes

- **Parameter layout:** removing 14 params changes the APVTS layout — bump
  `corruptrStateVersion` and add a migration branch in `setStateInformation`.
- **Randomizer:** implement on the message thread via `setValueNotifyingHost`
  (with begin/endChangeGesture per param) so hosts record the changes; snapshot
  all affected parameter values before applying for the one-level undo.
- **Feedback cap:** randomize `feedbackAmount` in normalized range mapping to
  0–10% of the parameter's real-world range; `feedbackDamping` may roll freely.
- **UI:** new CORRUPT box below the glitch + low-pass sections; needs a WebView
  → C++ action binding (non-parameter event) plus an UNDO affordance (small
  secondary button or same button shift-click — implementer's choice, keep simple).
- **Presets:** audit factory presets for any `performance*` values; strip on save.

## Backward Compatibility

- **Breaking:** host automation lanes on the 14 removed `performance*` parameters.
- **Non-breaking:** old sessions/presets load via state-version migration
  (performance values dropped silently).

## Testing Criteria

- [ ] All 14 `performance*` parameters gone from the APVTS; pluginval strictness 10 passes
- [ ] v1.0.x saved state loads cleanly (migration drops performance values, no crash)
- [ ] CORRUPT click randomizes only the agreed scope; 100-roll soak test confirms
      `feedbackAmount` ≤ 10% and filter + level params bit-identical before/after
- [ ] Undo restores the exact pre-click state (all randomized params)
- [ ] No clicks/blowups on randomize while audio is running (param smoothing holds)
- [ ] Factory presets unaffected; UI box renders below glitch/low-pass sections

## Success Metrics

Performance section fully gone, old projects still load, and every CORRUPT click
yields a safe, audibly different patch that can be undone in one click.
