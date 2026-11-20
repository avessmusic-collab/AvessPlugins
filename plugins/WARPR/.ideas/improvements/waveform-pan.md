# VOID - Waveform Pan (Drag + Shift+Wheel)

**Created:** 2026-11-20
**Type:** Enhancement
**Aspect:** UI
**Version Impact:** None — VOID is unreleased, stays 1.0.0 (same policy as KICKR)

## Current State

When zoomed into the waveform there is no discoverable way to move the visible window:

- Pan exists only as horizontal wheel delta (`Source/ui/public/index.html:1036`) — a two-finger
  sideways trackpad swipe. Invisible, and unreachable with a mouse wheel (vertical only = zoom).
- The ZOOM −/+ buttons zoom anchored at view center; no pan counterpart.
- The minimap strip drawn when zoomed (`index.html:1240`) is a 4px indicator, not interactive.
- Pointer-drag on the wave canvas is claimed only near START/END markers (0.02×span radius,
  `index.html:1279`) and slice edges (0.012×span, `index.html:1259`); dragging empty wave does
  nothing (clicking merely deselects a selected slice edge).

User (owner, uses both mouse and trackpad) zooms in and cannot move the view.

## Proposed Change

Two new pan gestures; existing gestures unchanged.

1. **Drag-to-pan on the waveform.** When zoomed (`WF.span < 0.999`), a pointerdown on empty wave
   area (i.e. not captured by a slice edge or START/END marker — those keep priority) starts a pan
   drag: the view scrolls under the hand (`WF.off -= dxNorm * WF.span`, clamped to `[0, 1−span]`).
   Cursor feedback: `grab` when zoomed and hovering empty wave, `grabbing` while dragging,
   existing `pointer` when not zoomed. Pointer capture as with the other drags.
2. **Shift+wheel = pan** for mouse users: in the wheel handler, `e.shiftKey` routes vertical
   `deltaY` into the existing deltaX pan branch instead of zoom.

Explicitly out of scope (owner decided): interactive minimap, ‹ › pan buttons, playhead
auto-follow (view only moves when the user moves it).

## Rationale

Zoom without pan is a dead end — you can only inspect whatever happens to be centered. Drag-to-pan
is the standard sampler convention (Kontakt, Simpler, Serum) and costs no screen space; Shift+wheel
covers mouse-only use where the current trackpad-swipe pan is physically unavailable.

## Implementation Notes

All in `Source/ui/public/index.html`; pure view state, no C++/DSP/parameter changes.

- `pointerdown` (`index.html:1255`): after slice-edge and marker hit tests fail, if
  `WF.span < 0.999` start `panDrag = { startX: e.clientX, startOff: WF.off }` + pointer capture,
  instead of returning.
- `pointermove`: when `panDrag` active,
  `WF.off = clamp(panDrag.startOff - (e.clientX - startX)/rect.width * WF.span, 0, 1 - WF.span)`,
  then `wfRequest(); fxRequest(); drawSample();` (same refresh trio as the wheel handler —
  `wfRequest` is already debounced 60ms, `fxRequest` throttled ~14Hz, so dragging stays cheap).
- `pointerup`/`pointercancel` (`envEnd`, `index.html:1301`): clear `panDrag`.
- Wheel handler (`index.html:1032`): `if (e.shiftKey)` treat `deltaY` as pan (reuse the deltaX
  branch). Note some OS/browser combos deliver shift+wheel *as* deltaX already — branch on
  `shiftKey` first so both paths land in pan.
- Cursor: update `cs.style.cursor` on zoom changes and during drag (`grab`/`grabbing`/`pointer`).
- Guard: drag-to-pan must NOT fire when not zoomed (span ≈ 1 — nothing to pan, and plain clicks
  on empty wave must keep doing nothing except deselect).
- Touch already works via pointer events (`touchAction: "none"` is set); one-finger drag on empty
  wave will pan on touchscreens for free.

## Backward Compatibility

Non-breaking. No parameters, no state format changes, no DSP. Marker/slice drag precedence
unchanged; double-click reset-to-full unchanged; trackpad sideways pan unchanged.

## Testing Criteria

- [ ] Zoomed in: drag on empty wave pans; view clamps at sample start/end; peaks refresh (fine
      `rangePeaks` detail appears after the debounce)
- [ ] Drag near a slice edge or START/END marker still moves the edge/marker, never pans
- [ ] Not zoomed: drag on empty wave does nothing (and still deselects a selected slice edge)
- [ ] Shift+wheel pans on a mouse; plain wheel still zooms at cursor; sideways trackpad swipe
      still pans
- [ ] Cursor shows grab/grabbing only when zoomed over empty wave
- [ ] Warp/shaper preview overlay stays aligned while panning (fxPeaks window follows)
- [ ] Double-click still resets to full view; slice add/remove dblclick behavior unaffected

## Success Metrics

Zoomed navigation feels like a hardware sampler: zoom in anywhere, grab the wave, slide to any
region, fine-place markers/slices — without ever resetting to full view to reposition.
