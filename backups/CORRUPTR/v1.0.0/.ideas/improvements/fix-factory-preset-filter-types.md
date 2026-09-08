# CORRUPTR - Fix Factory Preset Filter Types

**Created:** 2026-09-08
**Type:** Fix
**Aspect:** UI (factory preset bank)
**Version Impact:** PATCH

## Current State

Commit 218bc48 replaced the 8-type filter (`LP|HP|BP|Notch|Comb|Resonant LP|Resonant HP|Formant`)
with the 5-type Auto Filter engine (`Lowpass|Highpass|Bandpass|Notch|Morph`, indices 0–4),
but six factory presets in `Source/ui/public/index.html` still reference old indices:

| Preset | Old value | Old meaning | What loads now |
|---|---|---|---|
| The Screamer (line 2794) | `filterType:5` | Resonant LP | Morph (clamped) |
| Talk Box (line 2796) | `filterType:7` | Formant | Morph (clamped) |
| Acid Bath (line 2797) | `filterType:5` | Resonant LP | Morph (clamped) |
| Auto Wah (line 2798) | `filterType:5` | Resonant LP | Morph (clamped) |
| Vowel Movement (line 2800) | `filterType:7` | Formant | Morph (clamped) |
| Gate Chopper (line 2802) | `filterType:5` | Resonant LP | Morph (clamped) |

`setChoiceIndex(5)` normalizes to 1.25, clamps to 1.0 → index 4 (Morph). Since
`filterMorph` defaults to 0, Morph at 0 ≈ Lowpass — so all six load as a plain LP,
losing their intended character (especially the two Formant vocal presets).

Also stale: the `filterSlope` tooltip (line 2501) still says "(Comb/Formant unaffected)" —
those types no longer exist.

## Proposed Change

Remap the six presets to the new 5-type list:

- **Resonant LP presets** (The Screamer, Acid Bath, Auto Wah, Gate Chopper):
  `filterType:0` (Lowpass) — their existing high `filterResonance` values already
  supply the resonant character on the new SVF engine.
- **Formant presets** (Talk Box, Vowel Movement): `filterType:2` (Bandpass) with
  their existing high resonance for a vocal/talkbox band emphasis. Alternative:
  `filterType:4` (Morph) with a tuned `filterMorph` value if a sweepable vowel
  feel sounds better in the DAW.
- Update the `filterSlope` tooltip text to drop "(Comb/Formant unaffected)".

## Rationale

Factory presets are the first thing users audition; six of them currently load a
sound unrelated to their names (e.g. "Talk Box" is a plain lowpass). Zero DSP risk —
data-only change in the preset table.

## Implementation Notes

- File: `plugins/CORRUPTR/Source/ui/public/index.html` (preset table ~lines 2794–2802,
  tooltip line 2501).
- No C++ changes. `filterPickOutput()` already clamps out-of-range indices safely
  (verified — this is a sound-character bug, not a stability bug).
- Rebuild + reinstall Mac VST3/AU; Windows installer rebuild optional (cosmetic-level fix).

## Backward Compatibility

Non-breaking. DAW sessions saved with removed types (old index 4–7) already clamp
to Morph on load — inherent to the v12 filter rework, unchanged by this fix.

## Testing Criteria

- [ ] Each of the six presets loads with the remapped filter type shown on the filter tile
- [ ] Talk Box / Vowel Movement audibly emphasize a vocal band (not plain LP)
- [ ] Acid Bath / Auto Wah / The Screamer / Gate Chopper retain resonant LP character
- [ ] filterSlope tooltip no longer mentions Comb/Formant
- [ ] pluginval strictness 10 still passes

## Success Metrics

All factory presets produce their named character on the v12 filter engine.

## Audit Context (2026-09-08 bug check)

Full bug check performed the same day:
- pluginval 1.0.4, strictness 10, installed VST3: **SUCCESS** (thread safety, bus
  layouts, fuzz, state, editor lifecycle all pass).
- All other preset enum values validated in range: distortionAlgorithm (0–11),
  bitcrushMode (0–5), glitchMode (0–17), glitchBufferLength (0–8), lfoShape (0–8),
  modSlotSource (0–9), modSlotDestination (0–11), sequencer lanes (0–9).
- No stale Comb/Formant references in C++ — only the UI preset table + one tooltip.
