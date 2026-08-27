---
plugin: Transitionist
date: 2026-08-27
problem_type: ui_state_sync
component: webview
symptoms:
  - "Preset dropdown always shows 'Init' immediately after the plugin window opens, regardless of the plugin's actual restored/automated parameter values"
  - "Dropdown never updates in response to host automation, undo/redo, or a project reload - only updates when the user directly touches the preset bar itself"
root_cause: incomplete_state_sync
juce_version: 8.x
resolution_type: code_fix
severity: medium
tags: [webview, presets, state-sync, gui]
---

# Preset Dropdown Hardcoded to "Init" on Load Instead of Reflecting Real Parameter State

## Problem

Found during a general bug audit. An earlier, uncommitted edit to
`Source/ui/public/index.html` (made to fix an unrelated `<select>` repaint
issue) replaced the preset bar's dynamic "detect current preset, or show
Custom" logic with a hardcoded initializer:

```js
let currentIndex = 0;
selectEl.value = "0";
forceSelectRepaint(selectEl);
```

This means the dropdown always displays "Init" the moment the UI loads,
and is never re-evaluated afterward except when the user directly
interacts with the preset bar's own controls (select/prev/next). If a
saved project is reopened with different parameter values (e.g. a
previous session left on "Cavernous Hall", or the host restores automation
partway through a preset recall, or the user undoes a preset change), the
dropdown silently shows a name that does not match what's actually
loaded/sounding.

## Investigation

- Diffed the current (uncommitted) `index.html` against the prior committed
  version and confirmed the `findMatchingPresetIndex()` / "Custom" fallback
  function existed previously and was removed, not merely refactored.
- Confirmed the removal was a side effect of fixing a *different* bug (a
  WebKit `<select>` closed-label repaint quirk - the `forceSelectRepaint()`
  helper), not an intentional design decision to drop state-truthfulness.
- Verified all 5 relevant relay states (`transitionState`, `reverbState`,
  `delayState`, `dryWetState`, `delaySyncState`) already expose a
  `valueChangedEvent` that fires on every source of change (host
  automation, project reload, undo/redo, direct knob drag) - the mechanism
  needed to keep the dropdown honest was already available, just unused
  for this purpose after the earlier edit.

## Root Cause

An UI-truthfulness regression introduced as a side effect of an unrelated
bugfix: the dropdown's "reflect reality" logic was deleted instead of kept
alongside the repaint fix.

## Solution

Restored `findMatchingPresetIndex()` (epsilon-matched against all 8
`PRESETS` entries) and a `refreshSelectionDisplay()` function that:
- Shows the matching preset name if all 5 relevant parameters are within
  1 unit of a known preset.
- Inserts/selects a synthesized "Custom" option otherwise.
- Is registered as a `valueChangedEvent` listener on all 5 relevant relay
  states, so it re-runs on every real change, from any source - not just
  once at load.
- Is guarded by an `applyingPreset` flag so applying a preset via the bar's
  own prev/next/select controls doesn't cause a visible "Custom" flicker
  between each of the 5 individual `setNormalisedValue()` calls that make
  up one preset recall.

```js
function refreshSelectionDisplay() {
  if (applyingPreset) return;
  const match = findMatchingPresetIndex();
  if (match === currentIndex) return;
  // ...insert/remove "Custom" option, update selectEl.value, forceSelectRepaint()
}

transitionState.valueChangedEvent.addListener(refreshSelectionDisplay);
reverbState.valueChangedEvent.addListener(refreshSelectionDisplay);
delayState.valueChangedEvent.addListener(refreshSelectionDisplay);
dryWetState.valueChangedEvent.addListener(refreshSelectionDisplay);
if (delaySyncState) delaySyncState.valueChangedEvent.addListener(refreshSelectionDisplay);

refreshSelectionDisplay(); // once at setup, instead of hardcoding index 0
```

Applied identically to all three synced copies of this file:
`Source/ui/public/index.html`, `.ideas/mockups/v5-ui.html` (kept in sync
per project convention - `v5-ui.html` is the production HTML's
design-history source-of-truth copy).

## Verification

Rebuilt and reinstalled via `scripts/build-and-install.sh Transitionist
--reconfigure`; `pluginval --strictness-level 5` still SUCCESS (this bug
class is UI-only and not something pluginval's automated suite exercises -
verification here is code-review-based: confirmed the restored logic is a
strict superset of the previous session's repaint fix, not a reversion of it).

## Prevention

**Detection heuristic:** whenever removing or refactoring code that
listens to `valueChangedEvent` on a parameter relay state, check whether
that code's original job was "keep a derived UI element honest relative to
real parameter state" (dropdowns, mode indicators, computed labels) - a fix
for one symptom (a repaint glitch) can silently regress a different,
unrelated guarantee (state truthfulness) if the two concerns share the same
function.

**For this codebase generally:** any future WebView plugin with a
preset/quick-recall bar should register its "what preset am I on"
detection against the SAME relay `valueChangedEvent`s the knobs already
use, from the start, rather than only updating the display from the bar's
own button/select handlers.

## Related Issues

- `troubleshooting/gui-issues/webview-knob-missing-automation-gesture-bracketing-Transitionist-20260826.md` -
  a different preset-bar-adjacent fix from an earlier session (gesture
  bracketing on preset recall), unaffected by this bug/fix.
