---
plugin: Transitionist
date: 2026-08-26
problem_type: ui_layout
component: webview
symptoms:
  - "Knobs sound correct and update visually, but user can't reliably record automation in a DAW (Ableton)"
  - "Automation playback (pre-recorded envelopes) works fine - only live recording via dragging a control is affected"
  - "No error, warning, or console message of any kind - completely silent gap"
root_cause: api_misuse
juce_version: 8.x
resolution_type: code_fix
severity: high
tags: [webview, automation, gesture, ableton, vst3, gui]
---

# WebView Knobs Never Bracketed Automation Gestures (sliderDragStarted/Ended)

## Problem

User asked: "make sure that i can record knob automation in DAWs like
ableton." Investigation found that none of Transitionist's three WebView
knobs (THROW/SPACE/SWEEP) ever called `SliderState.sliderDragStarted()` or
`sliderDragEnded()` - the JS methods that bracket a value change as a single
automation "gesture" on the C++ side.

## Context

- Plugin: Transitionist (Stage 3 GUI, post-implementation live testing)
- All 3 knobs use `WebSliderRelay` + `WebSliderParameterAttachment`,
  correctly wired for value sync in both directions (drag updates the
  parameter, host automation/preset recall updates the knob)
- pluginval (`--strictness-level 5`) passed on both VST3 and AU, including
  its "Automation" test category, both before and after this fix

## Investigation

Checked whether this was project-wide by grepping every WebView plugin's
shipped `index.html` for the two method names:

```bash
grep -rln "sliderDragStarted\|sliderDragEnded" plugins/*/Source/ui/public/index.html
```

Only **one** plugin in the entire codebase (AngelGrain) called these methods
(for pointer-drag only, not for its wheel handler). Every other WebView
plugin - including Transitionist - was missing them entirely. This was
never caught because:

1. **pluginval's "Automation" test doesn't catch it.** It exercises the
   `AudioProcessorParameter` interface directly (calling `setValue`/
   `setValueNotifyingHost` style APIs), which doesn't require gesture
   bracketing to work - so the automated test suite gives a false sense of
   completeness for something it structurally cannot detect.
2. **Automation playback is unaffected.** A host applying a pre-recorded
   automation envelope just calls `setValue()` on the parameter - gesture
   bracketing only matters for the host to know when to *start recording* a
   new envelope from live user interaction (VST3 `beginEdit`/`endEdit`,
   Ableton's Touch/Latch record modes).
3. **The plugin sounds and looks completely correct during manual testing.**
   Dragging a knob changes the parameter and the audio immediately - there's
   no visible or audible symptom. The gap is invisible unless someone
   specifically tries to *record* automation from a live drag in a DAW that
   enforces proper touch semantics.

## Root Cause

`WebSliderRelay::Listener` (JUCE C++) turns two specific JS-side events into
`beginChangeGesture()`/`endChangeGesture()` calls on the underlying
parameter:

```cpp
// juce_audio_processors/utilities/juce_ParameterAttachments.h
void sliderDragStarted (WebSliderRelay*) override { attachment.beginGesture(); }
void sliderDragEnded   (WebSliderRelay*) override { attachment.endGesture(); }
```

But the JS-side `SliderState` class (in the JUCE-provided
`js/juce/index.js`, copied verbatim into every WebView plugin in this
codebase) only *emits* those events if the plugin's own knob-interaction
code explicitly calls `sliderState.sliderDragStarted()` /
`sliderState.sliderDragEnded()`. Nothing calls these automatically - a
custom WebView knob built from raw `mousedown`/`mousemove`/`mouseup`
listeners (as every plugin here does) has no reason to know these methods
exist unless someone reads the JS bridge source directly, since neither the
knob nor the relay throws a warning about it.

## Solution

Added calls to `sliderDragStarted()`/`sliderDragEnded()` at every point a
WebView control changes a `WebSliderRelay`-backed parameter:

```javascript
// Pointer drag - ONE gesture spanning the whole mousedown -> mouseup
function onPointerDown(e) {
  dragging = true;
  lastY = e.clientY;
  state.sliderDragStarted();
  window.addEventListener("mousemove", onPointerMove);
  window.addEventListener("mouseup", onPointerUp);
}

function onPointerUp() {
  dragging = false;
  state.sliderDragEnded();
}

// Wheel / keyboard / double-click - each is its OWN discrete gesture,
// matching native juce::Slider's own per-wheel-event ScopedDragNotification
knobEl.addEventListener("wheel", (e) => {
  e.preventDefault();
  state.sliderDragStarted();
  state.setNormalisedValue(/* ... */);
  state.sliderDragEnded();
});
```

Also applied to the plugin's preset-browser bar: recalling a preset sets 3
parameters at once, so each parameter's write is individually wrapped in
its own `sliderDragStarted()`/`setNormalisedValue()`/`sliderDragEnded()` -
otherwise selecting a preset while automation-record is armed in the host
would silently fail to write an envelope for that change too.

Files touched: `Source/ui/public/index.html` (shipped), plus the two mockup
copies that need to stay in sync (`v4-ui.html` production template,
`v4-ui-test.html` browser mockup - the latter's mock JUCE backend also
needed `sliderDragStarted`/`sliderDragEnded` no-op stubs added so the
browser mockup doesn't throw on the new calls).

## Verification

- `grep -c "sliderDragStarted\|sliderDragEnded" Source/ui/public/index.html`
  went from 0 to a positive count
- Rebuilt (`scripts/build-and-install.sh Transitionist`), reinstalled,
  `pluginval --strictness-level 5` re-run on both VST3 and AU: still SUCCESS
  (confirms the fix didn't regress anything the automated suite *can* check)
- True end-to-end confirmation (recording a live knob drag as automation in
  Ableton and confirming the envelope is written) requires manual DAW
  testing - not something coverable by pluginval or a script.

## Prevention

**For this codebase generally:** any future WebView knob/slider
implementation must call `sliderDragStarted()`/`sliderDragEnded()` around
every value-changing user interaction, not just drag. See Pattern #23 in
`troubleshooting/patterns/juce8-critical-patterns.md` for the full
before/after code pattern - this should be checked during Stage 5 (GUI)
code generation for every future plugin, not just when a user happens to
notice the symptom.

**Detection heuristic:** if a user reports being unable to record
automation in a DAW while manual control and automation *playback* both
work fine, check for `sliderDragStarted`/`sliderDragEnded` calls first -
this symptom profile is a strong signature of missing gesture bracketing.

## Related Issues

- See also: Pattern #16 (`WebView Knob Interaction - Relative Drag`) in
  `troubleshooting/patterns/juce8-critical-patterns.md` - covers the
  drag-math pattern this gesture bracketing wraps around, but did not
  originally mention gesture bracketing at all (now cross-referenced as
  Pattern #23).
