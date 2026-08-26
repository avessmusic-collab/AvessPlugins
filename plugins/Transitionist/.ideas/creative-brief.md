# Transitionist - Creative Brief

## Overview

**Type:** Audio Effect (Transition/Tail Effect)
**Core Concept:** A three-macro transition effect that throws incoming audio into an escalating delay+reverb wash which can be filter-swept away — for risers, breakdowns, and DJ-style transitions.
**Status:** 💡 Ideated
**Created:** 2026-08-25
**Revised:** 2026-08-26 — full signal-chain and parameter redesign (v2 concept). UI visual style (light skeuomorphic, established in mockup v3) carries forward unchanged; only the underlying DSP architecture and macro set changed.

## Vision

Transitionist throws the incoming audio into an escalating delay+reverb wash that builds tension, then lets it be filter-swept away — the classic riser/breakdown/DJ-transition move, distilled into three macro knobs.

**THROW** is the primary macro: it's simultaneously dry/wet, delay feedback, and reverb decay/size. Turning it up doesn't just add wetness — it makes the wash itself bigger and more self-sustaining. Near the top of its range, the reverb engages a freeze/hold mode (feedback → 1.0, damping → 0, input muted) for an infinite, automatable tail — the signature "throw and hold" gesture.

**SWEEP** is the bipolar DJ-style filter control — the "shwoosh." Center is fully open/transparent; turning counter-clockwise closes a 24dB lowpass, clockwise opens a 24dB highpass, with resonance rising toward both extremes for an aggressive sweep character in either direction.

**SPACE** links delay time (tempo-synced division) and reverb size together into one "room" macro, independent of THROW's decay/feedback role — short settings give a tight slapback with a small room; long settings give a cavernous wash with long echo spacing.

The wet path also runs through a subtle output glue stage (soft-clip/fast limiter, to catch feedback and freeze buildup) and a width stage (M/S widening, wet-only — dry stays mono-safe).

## Parameters

Three macro knobs, each displaying a live percentage-of-engagement readout in its center (LCD-style inset readout, per the v3 UI mockup).

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| THROW | 0–100% | 0% | Combined dry/wet, delay feedback amount, and reverb decay/size. Engages reverb freeze/hold (feedback→1.0, damping→0, input muted) near the top of the range for infinite tails. Saturation drive in the feedback loop scales minimally with this control. |
| SWEEP | -100% to +100% (bipolar, center = 0% = fully open) | 0% (center) | Bipolar control over the ladder filter. Center = transparent/no filtering. Negative (CCW) = 24dB lowpass closing in. Positive (CW) = 24dB highpass opening up. Resonance rises toward both extremes. |
| SPACE | 0–100% | 0% | Links delay time (tempo-synced division) and reverb size together — one "room" macro from tight slapback to cavernous wash. Independent of THROW's feedback/decay-amount role. |

## Signal Chain

**Wet path (fixed order):** Delay → Reverb → Bipolar DJ Filter → Output Glue → Width

The wet path result is blended against the dry signal via THROW's dry/wet role (dry stays mono-safe; only the wet path is widened).

### Stage details

**Delay**
- `juce::dsp::DelayLine` with Lagrange3rd interpolation, tempo-synced (division driven by SPACE).
- Feedback loop implemented manually (not a black-box feedback delay) — the loop must include, inside the loop path: a `tanh` waveshaper (minimal drive, scales slightly with THROW) and a one-pole lowpass (`FirstOrderTPTFilter`), so each repeat progressively darkens and thickens for analog/tape-like character.
- Feedback amount driven by THROW.

**Reverb**
- Long, modulated reverb tail. Decay/size driven jointly by THROW (decay/feedback amount) and SPACE (size).
- Freeze/hold mode: engages near the top of THROW's range — sets internal feedback → 1.0, damping → 0, and mutes further input — producing an infinite sustained tail until THROW is backed off.

**Bipolar DJ Filter**
- `juce::dsp::LadderFilter`, single bipolar control (SWEEP).
- Center = fully open (no filtering). CCW (negative) closes a 24dB lowpass. CW (positive) opens a 24dB highpass. Resonance increases toward both extremes.

**Output Glue**
- Gentle soft-clip / fast limiter on the wet path, positioned after the filter, to catch buildup from feedback and reverb freeze before it reaches Width/output.

**Width**
- M/S widening applied to the wet path only. Dry signal remains mono-safe/untouched.

## UI Concept

**Layout:** Three knobs (THROW, SPACE, SWEEP), left to right in that order — matching signal-flow position (delay+reverb macros first, filter macro last). No other user-facing controls. Window is 980×420, non-resizable. Each knob shows a live percentage-of-engagement readout in its center (SWEEP shows a bipolar readout, e.g. "-42%" / "+18%" / "0%" at center, with a fixed 12-o'clock tick marking its 0% detent and an arc that fills outward from center in either direction rather than a single CW sweep).
**Visual Style:** Light skeuomorphic hardware aesthetic, finalized as mockup v4 (2026-08-26). Outer bezel is a gray, subtly textured brushed-metal gradient (reusing the knob discs' own gray tones, darker than the panels so they pop off it) — not the flat white/off-white tried mid-iteration. Pale off-white/cool-silver inset panel cards with soft interior shadow sit inside it, photorealistic brushed-metal knob discs with fine ridged edges, subtle arc-track + pointer-dot indicator, cool blue-lavender ambient glow, small inset LCD-style digital percentage readout in each knob's center, bold geometric-sans logotype ("TRANSITIONIST") at top-center in dark blue-gray.
**Key Elements:** Minimal, transition-focused control surface; percentage reads directly on the knob face via the LCD-style window, not in a separate value pill beneath it.

## Use Cases

- Build-ups and breakdowns in electronic music production
- Filter-sweep style transitions between song sections (drop in/out)
- Live DJ-style performance FX on a send or master bus
- Automated "throw and freeze" riser effects going into a transition

## Inspirations

- Classic DJ mixer filter sweeps (Pioneer DJM-style bipolar filter send)
- Reverb freeze/infinite-hold effects (Valhalla-style freeze modes)
- Tape/analog echo throws with progressively darkening repeats, used in transition risers

## Technical Notes

- Wet-path chain order is fixed and non-reorderable: Delay → Reverb → Bipolar DJ Filter → Output Glue → Width.
- THROW is a triple-role macro: dry/wet blend, delay feedback amount, and reverb decay/size — plus it gates the reverb's freeze/hold engagement near max and lightly scales feedback-loop saturation drive. Stage 0 should determine the exact mapping curve and the freeze-engagement threshold.
- Delay feedback loop must be hand-built (not JUCE's built-in feedback delay abstraction) so a `tanh` waveshaper and `FirstOrderTPTFilter` one-pole lowpass can sit inside the loop itself, darkening/thickening each repeat.
- SWEEP is bipolar around a center detent (0% = fully open); Stage 0 should confirm the `LadderFilter` mode-switching approach between 24dB lowpass and 24dB highpass, and the resonance-rise curve toward each extreme.
- SPACE jointly maps tempo-synced delay division and reverb size — Stage 0 should determine a sensible shared mapping (e.g., a shared 0-100% index driving both) and confirm tempo sync via host BPM.
- Output Glue (soft-clip/fast limiter) exists specifically to contain buildup from delay feedback and reverb freeze — should be transparent at low THROW settings and only audibly engage under heavy buildup.
- Width (M/S widening) applies to the wet path only; dry path must remain mono-safe/unaffected.
- Parameter IDs from the v1/v2 UI mockups (`mix`, `duration`) are superseded by this redesign — Stage 0/UI mockup v4 should establish new parameter IDs (e.g. `throw`, `sweep`, `space`) matching the new macro set.

## Next Steps

- [ ] Update UI mockup to 3-knob layout (v4) — carry forward v3 visual style, add SPACE panel, rebind THROW/SWEEP/SPACE
- [ ] Start implementation (`/implement Transitionist`)
