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

**FINAL — locked to mockup v4** (`.ideas/mockups/v4-ui.yaml`, `meta.finalized: true`, 2026-10-29). GUI is **native JUCE** (user decision; no WebView). Phases 4.1–4.3 reproduce v4 with `juce::Graphics`.

**Layout:** 520×520 near-black canvas inside a 10px machined enclosure bezel (14px outer corner radius, mode-colored accent hairline). 190px central TENSION knob (270° arc, tick ring, 40px percent readout) over a radial pulse field (72 spikes, expanding rings, drifting particles, halo) driven by tension. MIX and OUTPUT as 56px knobs in the bottom corners, RISE MODE 3-way segmented pill bottom-center, SYNC LED pill top-right.
**Visual Style:** Premium minimalist; all knobs use a 9-layer 3D build (recessed well, contact shadow, bevel skirt, gradient dome, machined rings, bounce light, rim, specular; top-left light source). Depth and glow via gradients/strokes/clips only — everything drawable with juce::Graphics.
**Mode color themes** (crossfade 450 ms smoothstep, drives pulse field + arcs + inlays + LED + selector + bezel hairline): Slow Burn orange #ff8a3d → red #ff2d2a (glow_k 1.0); Cinematic (default) teal #2dd4c4 → electric blue #2b52ff (glow_k 1.15); Aggressive violet #b266ff → magenta #e62bc8 (glow_k 1.1). Pulse intensity gamma per mode: 1.25 / 1.0 / 0.75.
**Interactions:** relative-drag knobs (0.005/px, Shift = 0.1×), double-click reset; visualizer animation decoupled from audio (60 Hz timer reading telemetry atomics).

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
