# TENSION - Creative Brief

## Overview

**Type:** Audio Effect (Cinematic Tension/Riser)
**Core Concept:** A one-knob plugin that progressively transforms any audio into a cinematic riser — one oversized TENSION knob (0–100%) simultaneously drives noise, LFO modulation, filtering, stereo width, reverb, delay, feedback and saturation through coordinated nonlinear mappings.
**Status:** 💡 Ideated
**Created:** 2026-10-29

> **AUTHORITATIVE CONTRACT:** [implementation-spec.md](implementation-spec.md) — a complete
> user-provided build spec. This brief is a summary; the spec wins on any conflict.

## Vision

Insert on any track, draw one automation ramp, get a professional buildup. At 0% the plugin is transparent. Rising toward 100%, the source grows atmospheric, wider and more suspenseful until a huge-but-controlled climax: rising procedural noise, accelerating tempo-synced modulation, spectral movement, expansive delay/reverb. It also generates risers from silence via its internal noise engine.

Born from a DROP ideation session: the user wanted DROP's tension concept distilled to a single knob with constant (automation-defined) duration, no buttons, no phases — the knob is also the ride into the effect. Decided as a NEW plugin; DROP 1.0.0 stays untouched.

The result must sound like a purpose-built cinematic tension instrument, not a generic multi-FX chain — coordinated psychoacoustic progression, musical at 4/8/16/32-bar automation ramps.

## Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| TENSION | 0–100% | 0% | Master macro: nonlinear coordinated control of all DSP destinations; doubles as the effect's engagement/ride control |
| MIX | 0–100% | 100% | Dry/wet balance |
| OUTPUT | dB | 0 dB | Output trim |
| SYNC | On/Off | On | LFO/delay host-BPM sync vs free-running |
| RISE MODE | Slow Burn / Cinematic / Aggressive | Cinematic | Selects the master mapping profile (changes actual DSP curves, not just gain) |

## UI Concept

**Layout:** One oversized central circular TENSION knob (0–100% readout) with a reactive circular intensity visualization; compact MIX / OUTPUT / SYNC / RISE MODE controls. Nothing else.
**Visual Style:** Premium minimalist — near-black background, subtle gradients, clean typography, restrained orange/red accents.
**Key Elements:** Reactive circular visualizer around/behind the big knob; GUI animation decoupled from audio thread. (Spec §6 calls for native JUCE LookAndFeel; factory default is WebView — resolve in Stage 0.)

## Use Cases

- Automate TENSION 0→100% over 4–32 bars into a drop or scene change
- Generate risers from silence (no input needed) for cinematic sound design
- Subtle 10–40% settings as a suspense/atmosphere thickener on pads, vocals, drums

## Inspirations

- One-knob philosophy (Waves OneKnob school: one macro, coordinated internals)
- Cinematic riser/tension tools (riser generators built on noise + FX swells)
- DROP (this factory): dB-linear noise rise curves, reverb/delay tail architecture, output safety

## Technical Notes

- C++20, JUCE, CMake; APVTS; VST3 + Standalone (+ AU per factory amendment); 44.1–192 kHz; mono/stereo; RT-safe processBlock (no allocs/locks/IO)
- Modular DSP: TensionEngine, TensionMapping (smoothstep/exponential/musical curves per destination), LFOEngine (5 shapes, free/BPM-sync), NoiseGenerator (white/pink/filtered, deterministic for tests), FilterEngine, StereoWidth (M/S, low-mono-compatible), ReverbEngine, DelayEngine (ping-pong, feedback-safe), SaturationEngine, OutputSafety (NaN/Inf/peak scrubbing)
- Signal flow: INPUT → Input Gain → Pre-Filter → Modulation → Saturation → Stereo → Parallel Delay/Reverb → Output Safety → OUTPUT; noise is an independent parallel generator with its own filter/gain/modulation; independent dry/wet routing
- Per-destination smoothing times (no universal smoothing constant); zipper-free automation
- Full acceptance criteria and 7-phase dev process in implementation-spec.md §9–§11

## Next Steps

- [ ] Stage 0 planning (`/plan TENSION`) → architecture.md + plan.md
- [ ] Implementation (`/implement TENSION`)
