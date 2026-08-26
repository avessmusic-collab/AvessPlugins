# Transitionist - Creative Brief

## Overview

**Type:** Audio Effect (Transition/Tail Effect)
**Core Concept:** A channel-strip-style transition effect: one big macro knob drives the escalating build, with dedicated ping-pong delay and reverb sends, a dry/wet blend, and proper input/output gain staging with metering.
**Status:** 🚧 Implemented (v1.0.0, being re-architected)
**Created:** 2026-08-25
**Revised:** 2026-08-26 (v1) — 3-macro redesign (throw/space/sweep), implemented and shipped as v1.0.0.
**Revised:** 2026-08-26 (v2) — user-requested full control-surface redesign after using v1.0.0 in real DAW testing. Replaces the 3-macro/bipolar-filter concept with a more traditional, feature-explicit control set: one big "TRANSITION" hero knob, separate REVERB and DELAY knobs, a DRY/WET knob, and full INPUT/OUTPUT gain + metering. The bipolar DJ filter (SWEEP) and fixed Stereo Width stage are retired - not requested, and the new control set covers the same "transition" use case more directly.
**Revised:** 2026-08-27 (v3, this revision) — user-requested simplification: removed the INPUT/OUTPUT gain sliders and their level meters entirely, replaced with a single small VOLUME knob (-60 to +6dB, max 6dB boost), last stage in the chain, positioned on the right side of the plugin. No more per-stage input trim or metering - just one final output volume control.

## Vision

Transitionist is a transition/tail effect built around one big hero knob:
turn up TRANSITION and the wash (delay + reverb) grows bigger, more
self-sustaining, and eventually freezes into an infinite tail - the
signature "throw and hold" gesture, unchanged in spirit from v1. What's new
is that the *ingredients* of that wash are now separately dialed in with
their own dedicated knobs (REVERB, DELAY), rather than being bundled into
opaque macros, and the plugin now behaves like a proper channel-strip
utility with real input/output gain staging and metering, not just an FX
macro box.

**TRANSITION** (big knob, center, largest control on the plugin) is the
escalation/intensity macro: it scales reverb decay/size and delay feedback
intensity upward together, and gates the reverb's freeze/hold near the top
of its range. It does NOT itself control how much delay or reverb is
audible - that's REVERB/DELAY's job - it controls how *big and unstable*
those effects get as they're dialed in.

**REVERB** and **DELAY** are simple, direct "how much of this do I hear"
sends - each one is its own knob, each one is bypassed entirely at 0%. This
replaces v1's SPACE macro (which jointly and opaquely linked delay-time and
reverb-size) with something a user can reason about directly.

**DELAY** is now a proper stereo **ping-pong delay**: repeats alternate
between left and right, tempo-synced to the host's BPM via a dedicated
division selector (so the user can pick 1/8, 1/4, dotted, triplet, etc.
directly, rather than SPACE's opaque 0-100% division sweep from v1).

**DRY/WET** is a small, separate knob - a plain final blend between the
fully dry signal and the fully processed wet chain. This is distinct from
TRANSITION: TRANSITION shapes how intense/large the wet effect itself gets;
DRY/WET simply decides how much of that processed result you hear versus
the clean input.

**VOLUME** is a single small knob, last stage in the signal chain,
positioned on the right side of the plugin - simple final output level
control, -60 to +6dB (max 6dB boost). Replaces v2's separate INPUT/OUTPUT
gain sliders and their level meters entirely - no per-stage gain staging or
metering, just one final volume control after everything else.

## Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| TRANSITION | 0-100% | 0% | Big hero knob. Scales reverb decay/size and delay feedback intensity together; engages reverb freeze/hold near the top of its range for an infinite, automatable tail. |
| REVERB | 0-100% | 0% | Reverb send amount. 0% = reverb stage fully bypassed. |
| DELAY | 0-100% | 0% | Ping-pong delay send/feedback amount. 0% = delay stage fully bypassed. |
| DELAY SYNC | Choice: 1/16, 1/8T, 1/8, 1/8D, 1/4, 1/4D, 1/2, 1/2D, 1/1 | 1/4 | Tempo-synced ping-pong delay time, driven by host BPM (120 BPM fallback if host reports none). |
| DRY/WET | 0-100% | 50% | Final equal-power blend between the fully dry input and the fully processed wet chain. |
| VOLUME | -60 to +6 dB | 0 dB | Final output level, last stage in the chain. Small knob, right side of the plugin. Max 6dB boost. |

No level meters in this revision (removed along with v2's INPUT/OUTPUT
gain sliders they were paired with).

## Signal Chain

**Fixed order:** [dry tap] -> Ping-Pong Delay (tempo-synced, TRANSITION +
DELAY driven) -> Reverb (TRANSITION + REVERB driven, with freeze/hold) ->
Output Glue (fixed safety soft-clip/limiter) -> Dry/Wet Mix (against the
pre-effects dry tap) -> Volume (-60 to +6dB, last stage)

### Stage details

**Ping-Pong Delay**
- True alternating L/R ping-pong topology (not two independent synced
  delays): input is summed to mono and injected into delay line A only;
  line A's output feeds into line B (no fresh input); line B's output
  feeds back into line A. This produces genuine alternating bounces
  (A repeat, then B repeat from A's decayed content, then A again from B's
  decayed content, etc.), not simultaneous L+R echoes.
- Delay time from the DELAY SYNC division + host BPM (`AudioPlayHead::getPosition()->getBpm()`,
  JUCE 8 non-deprecated API, 120 BPM fallback), smoothed (~50ms ramp) to
  avoid clicks when the division or tempo changes.
- Feedback path includes a `tanh` waveshaper (drive scales with TRANSITION)
  and a one-pole lowpass (`FirstOrderTPTFilter`) per repeat, carrying
  forward v1's "each repeat progressively darkens/thickens" character.
- Feedback amount scales with DELAY (base) and TRANSITION (extra buildup),
  clamped well below runaway (~0.92 max). Stage wet/dry blend is driven
  purely by DELAY (0% = stage fully bypassed, regardless of feedback
  amount) so the DELAY knob behaves as a simple, predictable send.

**Reverb**
- `juce::dsp::Reverb`. Decay/size driven jointly by TRANSITION and REVERB
  (same spirit as v1's THROW+SPACE joint mapping, now with REVERB replacing
  SPACE's role for size).
- Freeze/hold: engages near the top of TRANSITION's range (smoothstep
  90%->100%, same curve as v1) - feedback -> 1.0, damping -> 0, new input
  muted (but the already-frozen tail keeps sustaining).
- Stage wet/dry blend driven purely by REVERB (0% = stage fully bypassed).

**Output Glue**
- Fixed internal safety net (not a user parameter): soft-clip
  (`juce::dsp::WaveShaper`, tanh) + `juce::dsp::Limiter`, positioned after
  the reverb stage to catch buildup from delay feedback + reverb freeze
  before the final dry/wet blend. Carried forward unchanged from v1.

**Dry/Wet Mix**
- Equal-power crossfade between the Output-Glue-processed wet chain and
  the ORIGINAL dry tap (captured before any effects stage) - driven by DRY/WET.

**Volume**
- Plain dB gain, -60 to +6dB (max 6dB boost), applied LAST - the final
  number that leaves the plugin. Small knob, right side of the plugin.

## Retired From v1

- **SWEEP / Bipolar DJ Filter:** not requested in this redesign; removed
  entirely (`juce::dsp::LadderFilter` dual-instance crossfade DSP and the
  bipolar arc-fill/center-detent-tick UI pattern are both retired).
- **SPACE:** replaced by DELAY SYNC (explicit tempo division choice) +
  DELAY's own send amount - the "opaque 0-100% joint delay-time/reverb-size
  macro" concept is gone in favor of direct, separately-labeled controls.
- **Stereo Width (M/S):** not requested; removed. Ping-pong delay itself
  provides the stereo interest this plugin needs.
- **Reverb Modulation (LFO micro-delay):** a v1 "keep the tail alive"
  enhancement, dropped in this redesign to keep the new, larger control
  surface's DSP focused and directly traceable to what was requested.
- **INPUT/OUTPUT gain sliders + level meters (v2):** removed in v3 per user
  request - replaced with a single small VOLUME knob, last in chain, right
  side of the plugin. No more per-stage gain staging or metering.

## UI Concept

**Layout:** Left to right: DELAY (knob + sync dropdown) - REVERB (knob) -
TRANSITION (big hero knob, center, largest) - DRY/WET (small knob) -
VOLUME (small knob, rightmost, last in chain). Quick preset bar retained
at top, above the control row. Window is 1010x440 (see
`.ideas/mockups/v5-ui.yaml` for exact dimensions).
**Visual Style:** Carries forward the v3/v4 light skeuomorphic hardware
aesthetic UNCHANGED: gray textured brushed-metal outer frame, pale
off-white/cool-silver inset panel cards, photorealistic brushed-metal knob
discs, cool blue-lavender ambient glow, inset LCD-style readouts, geometric-
sans logotype. TRANSITION's panel card is visibly the largest (matches its
role as the hero control); all panel cards share the same height for a
consistent "rack strip" look, with each knob vertically centered in its own
card regardless of diameter. VOLUME uses the same small-knob tier as
DRY/WET, with a signed dB LCD readout instead of a percentage.
**Key Elements:** All 5 controls are now knobs (no sliders) - VOLUME shows
a signed dB readout (e.g. "+3.0 dB", "-12.0 dB") instead of a percentage,
the only knob in this plugin that does.

## Use Cases

- Build-ups and breakdowns in electronic music production
- Ping-pong delay transitions synced to project tempo
- Automated "throw and freeze" riser effects going into a transition
- General-purpose send/insert utility with a simple final volume trim

## Inspirations

- Classic ping-pong delay pedals/plugins (tempo-synced, alternating L/R)
- Reverb freeze/infinite-hold effects (Valhalla-style freeze modes)
- Simple single-knob output volume convention (common on hardware sends/inserts)

## Technical Notes

- Signal chain is fixed and non-reorderable: Ping-Pong Delay -> Reverb ->
  Output Glue -> Dry/Wet Mix -> Volume.
- TRANSITION drives escalation/character (reverb decay+freeze, delay
  feedback intensity) but NOT the audibility of either effect - REVERB and
  DELAY each independently gate their own stage's wet/dry blend to 0% at
  their minimum, regardless of TRANSITION or feedback amount.
- Ping-pong delay MUST use the true alternating-bounce topology (fresh
  input only into line A, cross-fed into line B) - not two independently
  synced L/R delays, which would produce simultaneous stereo echoes rather
  than a true bounce.
- DELAY SYNC choices: 1/16, 1/8T, 1/8, 1/8D, 1/4, 1/4D, 1/2, 1/2D, 1/1
  (shortest to longest), default 1/4. Tempo sync via
  `AudioPlayHead::getPosition()->getBpm()`, 120 BPM fallback.
- VOLUME range: -60 to +6 dB (max 6dB boost), default 0 dB, linear-in-dB
  parameter applied as the final stage, after the dry/wet blend.
- No level meters or per-stage gain staging in this revision (v2's INPUT/
  OUTPUT sliders + meters were removed entirely).
- Output Glue (soft-clip/fast limiter) remains a fixed internal safety net,
  not exposed as a parameter - same rationale as v1 (catches delay
  feedback + reverb freeze buildup).

## Next Steps

- [x] v1.0.0 shipped with 3-macro concept (throw/space/sweep) - superseded
- [x] v2: 7-parameter control-surface redesign (transition/reverb/delay/
      delaySync/dryWet/inputGain/outputGain) - implemented, then superseded
- [x] v3: removed INPUT/OUTPUT gain sliders + meters, added single VOLUME
      knob (-60..+6dB, last in chain, small, right side) - implemented,
      rebuilt, pluginval-verified on both VST3 and AU
- [ ] Live DAW verification of the full v3 control set (automation
      recording, VOLUME boost/cut behavior, preset bar dropdown display -
      see .continue-here.md for a known open cosmetic issue with the
      latter)
