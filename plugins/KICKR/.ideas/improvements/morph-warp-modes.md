# KICKR - Morph Warp Modes

**Created:** 2026-09-02
**Type:** Feature
**Aspect:** DSP / Parameters / UI
**Version Impact:** MINOR (stays 1.0.0 per KICKR's pre-release versioning policy — see NOTES.md)

## Current State

The `morph` parameter (0.0-1.0) drives a single fixed algorithm: a CZ-style phase-skew
of the body oscillator that pushes the attack toward a soft saw while the tail always
relaxes back to a clean sine (`KickVoice::setMorph`, `DSP/KickVoice.cpp`). There is one
knob, one behavior — no way to choose a different character of warp.

## Proposed Change

Turn Morph into a Serum-Warp-style feature: a **mode selector** plus the existing
**Morph knob repurposed as a per-mode amount/depth control**.

**New parameter:** `morphMode` — discrete choice, 8 modes:

1. **Bend/Skew** — today's existing CZ-style phase-skew behavior, unchanged. Default mode.
2. **Sync** — hard-syncs the body oscillator to a faster internal rate on the attack for extra growl/harmonics.
3. **Fold** — wavefolds the body oscillator for aggressive, metallic harmonic content.
4. **FM** — self-FM using a second internal oscillator at a fixed musical ratio (e.g. 2x/3x) of the body pitch, for a dirty/clangy attack.
5. **FM-from-Sample** — same FM structure, but the modulator is the plugin's existing Sample layer instead of an internal oscillator (cross-layer modulation — Sample layer output feeds the body osc's phase/frequency).
6. **PD** — classic phase-distortion (Casio CZ-style resonant sweep), distinct curve shape from Bend/Skew.
7. **AM** — amplitude-modulates the body oscillator with a fixed-ratio internal oscillator.
8. **RM** — ring-modulates the body oscillator with a fixed-ratio internal oscillator (bipolar multiply, distinct from AM).

**Modulator source (FM/AM/RM):** fixed internal ratio oscillator, pitch-tracked to the
body oscillator's fundamental. No new user-facing ratio parameter in this pass.

**Amount scaling:** each mode is tuned so `morph = 1.0` (max) lands at a musical/tame
ceiling rather than a fully extreme/broken extreme — consistent with how the current
Bend/Skew mode is deliberately gentle (see `PresetManager.cpp` Randomize range comment:
"gentle by design"). Fold, FM, FM-from-Sample and RM in particular need their curves
scaled down from a "textbook" full-range implementation to stay usable on a kick body.

**UI:** a small text-label selector (matching the existing `character` curve-name
readout pattern already used for the distortion morph) sits above/beside the Morph
knob. Clicking/dragging cycles through the 8 mode names. The knob itself keeps its
current size, position, and violet color (`pal::violet`) — only a label row is added.

**Mode-switch behavior:** the amount (morph) value is NOT reset or per-mode-remembered
when `morphMode` changes — the same 0-1 value carries over and is reinterpreted under
the newly selected mode's curve, matching how Serum's Warp knob behaves across mode
changes.

## Rationale

The current Morph knob has exactly one algorithm. The user wants Serum-Warp-style
variety — multiple distinct waveshaping/modulation characters selectable from one
control pair, to broaden the sonic range of kick attacks (from clean CZ-skew through
aggressive folding, FM, and ring-mod textures) without adding a wall of new knobs.

## Implementation Notes

**Files likely touched:**
- `Source/Parameters/ParameterIDs.h` — add `morphMode` id
- `Source/Parameters/ParameterLayout.h` — add `morphMode` as `AudioParameterChoice` (8 choices), keep existing `morph` as the amount param (rename its description, not its id, to avoid breaking automation)
- `Source/Parameters/ParameterDescriptions.h` — update `morph` description to describe "amount for the selected Morph mode"; add `morphMode` description
- `Source/DSP/KickVoice.h/.cpp` — `setMorph` becomes mode-aware; likely needs `setMorphMode(int)` plus 8 branch implementations (or a strategy/function-pointer per mode) inside the body oscillator's per-sample processing
- `Source/DSP/KickEngine.cpp` — read new `pMorphMode` raw parameter alongside `pMorph`, pass both into the voice each block (mirrors how `snap.morph01` / `v.setMorph(...)` currently flows)
- **FM-from-Sample specifically** needs a cross-layer read: the body oscillator's per-sample processing needs access to the Sample layer's current output, which today are independent, mixed-later layers (`6 layers ... -> Mix`, per NOTES.md). This is the one mode that changes the signal-flow architecture, not just the body oscillator's internals — flag for extra design attention during implementation.
- `Source/PluginEditor.h/.cpp` — add the mode-label selector control near `morphKnob`/`layoutMorph`, wire a new attachment for `morphMode`
- `Source/Presets/PresetManager.cpp` — existing presets that set `morph` continue to work unchanged (see Backward Compatibility); Randomize/Mutate ranges may want a `morphMode` entry too (open question for implementation time — could randomize mode, or leave mode fixed and only randomize amount)

**Bind count:** adding `morphMode` increases the APVTS-editor binding count past the current `jassert (boundParamCount == 60)` in `PluginEditor.cpp:320` — that assertion needs updating to 61.

## Backward Compatibility

Non-breaking. `morphMode` defaults to **Bend/Skew**, which is bit-for-bit today's
existing behavior — so every existing preset and any DAW automation on `morph` alone
sounds identical to today unless the user explicitly changes `morphMode`. (KICKR is
also still pre-1.0-public-release per the project's versioning policy, so strict
backward compatibility isn't a hard constraint regardless — see NOTES.md.)

## Testing Criteria

- [ ] For each of the 8 modes: no NaN/Inf output across all 4 oversampling factors (1x/2x/4x/8x)
- [ ] For each of the 8 modes: output stays bounded (no runaway) under machine-gun retrigger, mirroring the existing morph stress test in `RunTests.cpp` (~line 366-393)
- [ ] `morphMode = Bend/Skew, morph = 0` still produces the exact pre-existing "clean sine" output (regression check against current behavior)
- [ ] Instant `morphMode` automation changes mid-note don't produce clicks/discontinuities (mirrors the existing instant-morph-jump smoothing test at `RunTests.cpp:1780`)
- [ ] `morphMode` round-trips correctly through session save/load and preset save/load
- [ ] `boundParamCount` assertion updated to 61 and passes
- [ ] FM-from-Sample mode behaves sanely when the Sample layer is empty/disabled (must not produce silence-modulated garbage or NaN — needs an explicit fallback, e.g. treat missing sample as modulator amplitude 0)

## Success Metrics

All 8 modes are selectable, each produces an audibly distinct, musically usable
character across the amount range, existing presets/automation are unaffected by
default, and the full KICKR test suite (existing + new morph-mode tests) passes.
