# KICKR - Pitch Module + Filter Slope Selector

**Created:** 2026-09-07
**Type:** Enhancement
**Aspect:** DSP + Parameters + UI
**Version Impact:** Internal (KICKR stays at 1.0.0 until public release per versioning policy)

## Current State

**PUNCH module (UI):** Hero slot holds the large red MACRO knob (`macroPunch` — one-knob
macro driving sharper transient, snappier pitch drop, more click). Footer holds two small
knobs: T ATK (`transientAttack`) and T SUS (`transientSustain`).

**Pitch envelope:** Already exists in DSP (`Source/DSP/PitchEnvelope.h`) and is exposed as
three small knobs buried in the BODY module footer:
- `pitchStart` "P START" 1–10x (start ratio — this *is* the envelope amount)
- `pitchTime` "P TIME" 5–500 ms
- `pitchCurve` "P CURVE" 0–1 continuous (k = 0.6 + c·8.4 exponential family)

**Master filter:** `Source/DSP/MasterFilter.h` — single `juce::dsp::StateVariableTPTFilter`,
LP/HP, fixed 12 dB/oct. Parameters: `filterOn`, `filterType`, `filterFreq`, `filterRes`.

## Proposed Change

### 1. PUNCH module → PITCH module

The Punch module is repurposed as a dedicated pitch-envelope module:

- Section label changes from "PUNCH" to "PITCH" (keep red palette).
- The three pitch knobs move out of the BODY footer into this module:
  - **AMOUNT** (`pitchStart`, relabeled) — pitch-envelope depth, independently adjustable, 1–10x unchanged.
  - **TIME** (`pitchTime`) — pitch-drop time, range stays 5–500 ms (user decision: keep full range; 5–100 ms is the sweet spot the existing 0.3 skew already favors).
  - **CURVE** (`pitchCurve`) — stays a continuous knob morphing the drop shape from logarithmic (0) through to exponential (1). See implementation notes for the remap.
- **MACRO (punch) knob is removed from the UI.** Recommend keeping the `macroPunch` parameter in the layout (hidden) so old sessions/presets still load; its DSP contribution can stay as-is at default or be neutralized — decide during implementation.
- **T ATK and T SUS stay** in the module footer but rendered **smaller** than the current Small knob size (may need a new `KSize` tier or scaled variant).
- Layout: one of the three pitch knobs (suggest AMOUNT) can take the hero position vacated by MACRO.

### 2. Master filter slope selector

- New Choice parameter `filterSlope`: **12 / 18 / 24 dB per octave, default 18** (user decision).
- Applies to both LP and HP modes.
- UI: slope selector on the FILTER page of the scope, next to the LP/HP type control.

## Rationale

The one-knob Punch macro overlaps what the pitch envelope already does, while the actual
pitch controls are hidden in the BODY footer. Promoting the pitch envelope to its own
module gives direct, independent control over the kick's defining transient (drop depth,
time, shape). A slope choice on the master filter turns it from a tone-tilt into a proper
sculpting filter (steeper slopes for clean sub isolation / rumble removal).

## Implementation Notes

**Pitch curve (log→exp morph):** Current mapping is one exponential family with variable k.
To honor log↔exp:
- c = 0 → logarithmic-feel settle (slow start, fast end — e.g. invert the easing: `1 - exp-curve(1-tau)`)
- c = 0.5 → ~linear drop
- c = 1 → sharp exponential snap (current high-k behavior)
Keep the ratio/log-domain frequency math (`f = fund · start^e(tau)`) — never lerp Hz.
Existing presets keep their `pitchCurve` values; audible change at low values is accepted.

**Filter slope cascade:**
- 12 dB: current single `StateVariableTPTFilter` (unchanged path — bit-exact at 12 dB setting).
- 24 dB: two cascaded SVFs; resonance on one stage, second at Butterworth-ish Q (~0.707) to avoid double-resonance blowup.
- 18 dB: SVF (12 dB, carries resonance) + first-order TPT one-pole (6 dB).
- Reset filter state on slope change to avoid stale-state clicks; smooth or fade if a click is audible when switching while audio runs.
- `filterSlope` is a new parameter appended to the layout (new ParameterID version) — old sessions load with the default (18 dB). Note: old sessions with filter ON will sound slightly steeper than before (was 12); accepted by user (default 18 chosen deliberately).

**Files touched (expected):**
- `Source/Parameters/ParameterIDs.h`, `ParameterLayout.h`, `ParameterDescriptions.h` — add `filterSlope`, relabel pitchStart display name.
- `Source/DSP/MasterFilter.h/.cpp` — cascade stages + slope switch.
- `Source/DSP/PitchEnvelope.h` — curve remap (log↔exp morph).
- `Source/DSP/KickEngine.cpp` — pass slope param; possibly neutralize macroPunch pitch contribution.
- `Source/PluginEditor.cpp/.h` — module relabel, knob moves, smaller T ATK/T SUS, remove MACRO knob, FILTER page slope control.
- `Source/UI/FilterDisplay.*` — filter curve drawing should reflect the selected slope.

## Backward Compatibility

- **Non-breaking for session load:** `macroPunch` parameter retained (hidden); `filterSlope` added with default.
- **Sound-breaking (accepted):** existing sessions with filter enabled shift from 12 → 18 dB/oct unless slope is stored per-preset default 12 for old data (optional migration: if plugin state predates `filterSlope`, force 12). Recommend implementing this migration guard — cheap and preserves old mixes.
- Factory presets should be re-auditioned after the pitch-curve remap.

## Testing Criteria

- [ ] Filter slope selector switches 12/18/24 on both LP and HP; measured roll-off matches (pluginval + sine-sweep render).
- [ ] No clicks/zipper when switching slope or type during playback.
- [ ] 12 dB setting sounds identical to pre-change filter (A/B null test or close).
- [ ] Pitch AMOUNT/TIME/CURVE knobs live in the PITCH module; CURVE at 0 gives log-style settle, at 1 gives sharp exponential snap.
- [ ] T ATK / T SUS render smaller but remain usable; MACRO knob gone from UI.
- [ ] Old session/preset with `macroPunch` data loads without error.
- [ ] pluginval passes at strictness 10; DAW smoke test (user tests in DAW before commit).

## Success Metrics

Pitch envelope is fully controllable from its own module (depth, time, shape independent),
the punch macro knob is gone, and the master filter offers 12/18/24 dB slopes defaulting
to 18 — all verified in a DAW session with old presets still loading.
