# Transitionist - Creative Brief

## Overview

**Type:** Audio Effect (Transition/Tail Effect)
**Core Concept:** A channel-strip-style transition effect: one big macro knob drives the escalating build, with dedicated ping-pong delay and reverb sends, a dry/wet blend, and proper input/output gain staging with metering.
**Status:** 🚧 Implemented (v1.0.0, being re-architected)
**Created:** 2026-08-25
**Revised:** 2026-08-26 (v1) — 3-macro redesign (throw/space/sweep), implemented and shipped as v1.0.0.
**Revised:** 2026-08-26 (v2, this revision) — user-requested full control-surface redesign after using v1.0.0 in real DAW testing. Replaces the 3-macro/bipolar-filter concept with a more traditional, feature-explicit control set: one big "TRANSITION" hero knob, separate REVERB and DELAY knobs, a DRY/WET knob, and full INPUT/OUTPUT gain + metering. The bipolar DJ filter (SWEEP) and fixed Stereo Width stage are retired - not requested, and the new control set covers the same "transition" use case more directly.

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

**INPUT** and **OUTPUT** are proper gain-staging controls: a vertical
slider plus a live level meter for each, at the left and right edges of the
plugin, matching signal flow (gain in on the left, gain out on the right).

## Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| TRANSITION | 0-100% | 0% | Big hero knob. Scales reverb decay/size and delay feedback intensity together; engages reverb freeze/hold near the top of its range for an infinite, automatable tail. |
| REVERB | 0-100% | 0% | Reverb send amount. 0% = reverb stage fully bypassed. |
| DELAY | 0-100% | 0% | Ping-pong delay send/feedback amount. 0% = delay stage fully bypassed. |
| DELAY SYNC | Choice: 1/16, 1/8T, 1/8, 1/8D, 1/4, 1/4D, 1/2, 1/2D, 1/1 | 1/4 | Tempo-synced ping-pong delay time, driven by host BPM (120 BPM fallback if host reports none). |
| DRY/WET | 0-100% | 50% | Final equal-power blend between the fully dry input and the fully processed wet chain. |
| INPUT GAIN | -24 to +24 dB | 0 dB | Input trim, applied before all processing. Drives the INPUT level meter. |
| OUTPUT GAIN | -24 to +24 dB | 0 dB | Output trim, applied after the dry/wet blend. Drives the OUTPUT level meter. |

Two live level meters (INPUT, OUTPUT) are UI-only (not automatable
parameters) - peak level in dB, updated ~30Hz from the audio thread via an
atomic + editor Timer, per this codebase's established VU-meter pattern
(`troubleshooting/patterns/juce8-critical-patterns.md` Pattern #20).

## Signal Chain

**Fixed order:** Input Gain -> [dry tap] -> Ping-Pong Delay (tempo-synced,
TRANSITION + DELAY driven) -> Reverb (TRANSITION + REVERB driven, with
freeze/hold) -> Output Glue (fixed safety soft-clip/limiter) -> Dry/Wet Mix
(against the pre-effects dry tap) -> Output Gain -> [output level meter]

### Stage details

**Input Gain**
- Plain dB gain applied first. Feeds the INPUT meter (peak, post-gain) and
  the dry tap used for the final dry/wet blend.

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
  the ORIGINAL dry tap (captured right after Input Gain, before any
  effects stage) - driven by DRY/WET.

**Output Gain**
- Plain dB gain applied last. Feeds the OUTPUT level meter (peak,
  post-gain) - the final number that leaves the plugin.

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

## UI Concept

**Layout:** Left to right: INPUT (vertical slider + meter) - DELAY (knob +
sync dropdown) - TRANSITION (big hero knob, center, largest) - REVERB
(knob) - DRY/WET (small knob) - OUTPUT (vertical slider + meter). Quick
preset bar retained at top, above the control row (from v4). Window
resized to fit the wider control set (see `.ideas/mockups/v5-ui.yaml` for
exact dimensions).
**Visual Style:** Carries forward the v3/v4 light skeuomorphic hardware
aesthetic UNCHANGED: gray textured brushed-metal outer frame, pale
off-white/cool-silver inset panel cards, photorealistic brushed-metal knob
discs, cool blue-lavender ambient glow, inset LCD-style readouts, geometric-
sans logotype. TRANSITION's panel card is visibly the largest (matches its
role as the hero control); all panel cards share the same height for a
consistent "rack strip" look, with each knob vertically centered in its own
card regardless of diameter.
**Key Elements:** INPUT/OUTPUT are channel-strip-style vertical modules
(slider + live peak meter, LCD dB readout) rather than knobs, visually
distinct from the four rotary controls but still built from the same
material/color language (LCD, brushed-metal-adjacent gradients).

## Use Cases

- Build-ups and breakdowns in electronic music production
- Ping-pong delay transitions synced to project tempo
- Automated "throw and freeze" riser effects going into a transition
- General-purpose send/insert utility with proper gain staging (input/output
  metering makes it usable as a more everyday effect, not just a macro box)

## Inspirations

- Classic ping-pong delay pedals/plugins (tempo-synced, alternating L/R)
- Reverb freeze/infinite-hold effects (Valhalla-style freeze modes)
- Channel-strip gain staging conventions (input/output trim + meters)

## Technical Notes

- Signal chain is fixed and non-reorderable: Input Gain -> Ping-Pong Delay
  -> Reverb -> Output Glue -> Dry/Wet Mix -> Output Gain.
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
- INPUT/OUTPUT gain range: -24 to +24 dB, default 0 dB, linear-in-dB
  parameter (not a raw gain multiplier) for natural fader feel.
- Level meters are peak (not RMS), computed via `buffer.getMagnitude()`,
  converted to dB, floored at -100dB, stored in an `std::atomic<float>` and
  read by the editor's Timer at 30Hz - matches this codebase's established
  VU-meter pattern (see TapeAge for a working reference implementation).
- Output Glue (soft-clip/fast limiter) remains a fixed internal safety net,
  not exposed as a parameter - same rationale as v1 (catches delay
  feedback + reverb freeze buildup).

## Next Steps

- [x] v1.0.0 shipped with 3-macro concept (throw/space/sweep) - superseded by this revision
- [ ] Update UI mockup to the new 6-control layout (v5) - carry forward v3/v4 visual style
- [ ] Update parameter-spec.md with the new 7-parameter set (v2)
- [ ] Re-implement DSP (Stage 2) for the new signal chain
- [ ] Re-implement GUI (Stage 3) with new relays/attachments + meter Timer
- [ ] Rebuild, reinstall, re-verify (pluginval + manual DAW test)
