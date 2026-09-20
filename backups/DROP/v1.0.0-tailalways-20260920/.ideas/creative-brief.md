# DROP - Creative Brief

## Overview

**Type:** Effect (Musical Transition Engine)
**Core Concept:** A dedicated transition engine that turns a normal musical passage into BUILD → TENSION → VACUUM → IMPACT → RECOVERY — "give me a boring loop, tell me where the drop happens, and DROP creates the tension before it and the impact at the drop."
**Status:** 💡 Ideated
**Created:** 2026-09-13

> **Full specification:** This brief is a summary. The complete, authoritative implementation spec (module-by-module requirements, real-time safety rules, testing plan, phase breakdown) lives in [implementation-spec.md](implementation-spec.md). Stage 0 planning MUST read that file.

## Vision

DROP is NOT a generic multi-FX rack. Its identity is a musical transition engine where all effects are coordinated over time by a central **TransitionEngine** and **IntensityBudget** instead of running independently. The key differentiator: total effect intensity is normalized to 1.0 and *distributed* across modules by Style and transition progress, so a transition sounds intentional rather than like every effect maxed simultaneously.

Signature moment: the **Vacuum** — a very short, click-free, sample-accurate near-silence immediately before the impact. The most important contrast is the disappearance right before the return. The impact emphasizes the *original source transient* (clean return + optional short sub/noise accents), never a giant pile of effects.

Architecture: Input → musical analysis → TransitionEngine → IntensityBudget → coordinated DSP → Vacuum → ImpactEngine → Recovery.

**Design priority order:** 1. Timing, 2. Musical tension, 3. Contrast, 4. Impact, 5. Audio quality, 6. CPU efficiency, 7. UI simplicity, 8. Feature count.

## Parameters

Only high-value parameters are exposed (JUCE APVTS). Advanced per-module DSP internals stay internal, driven by Style/IntensityBudget.

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| Trigger | Bool/momentary | Off | Fires the transition (manual, MIDI note, or host automation) |
| Build Length | 1/2/4/8/16/32 bars | 16 bars | Transition duration, tempo-synced (32 appended 2026-09-17, POST-1.0) |
| Style | 16 DropStyles | Clean EDM | Data-driven effect-weight/curve/behavior profile |
| Intensity | 0–100% | 50% | Overall transition intensity (budget scale) |
| Tension | 0–100% | 50% | Tension curve emphasis (most tension near the end by default) |
| Chaos | 0–100% | 0% | Deterministic, seeded, musically-constrained variation |
| Impact | 0–100% | 50% | Impact strength (sub, burst, expansion, tail) |
| Filter | 0–100% | macro | Advanced: filter module weight |
| Width | 0–100% | macro | Advanced: M/S width module weight |
| Pitch | 0–100% | macro | Advanced: pitch rise/fall module weight |
| Gate | 0–100% | macro | Advanced: tempo-synced gate weight |
| Stutter | 0–100% | macro | Advanced: buffer-repeat stutter weight |
| Delay | 0–100% | macro | Advanced: tempo-synced stereo delay weight |
| Reverb | 0–100% | macro | Advanced: impact/recovery reverb weight |
| Distortion | 0–100% | macro | Advanced: saturation module weight |
| Noise | 0–100% | macro | Advanced: noise riser weight |
| Digital | 0–100% | macro | Advanced: bit/SR crush weight (added as 19th param per architecture.md resolution) |
| Dry/Wet | 0–100% | 50% | Global mix (default 100%→50% 2026-09-18, POST-1.0; factory presets re-authored to 50% in lockstep) |
| Noise Type | STYLE/WHITE/PINK/BROWN/SWEEP/SAW/SHEPARD | WHITE | **POST-1.0 (2026-09-18):** riser source selection, grounded in riser sound-design research (default STYLE→WHITE same day) — see [parameter-spec.md](parameter-spec.md)'s `noiseType` entry |
| Output | -inf–+12 dB | 0 dB | Output gain |
| Tail | 0–100% | 0.70s (46.91%) | **POST-1.0 (2026-09-16, default 35%→0% 2026-09-17):** ring-out length of the Recovery state (0% = clean ~5ms cut, else ~0.15s–4.0s exponential mapping); overrides the Style's recovery timing — see [parameter-spec.md](parameter-spec.md)'s `tail` entry and `plugins/DROP/CHANGELOG.md` |
| Noise Tension | 0–100% | 0% | **POST-1.0 (2026-09-17, default 50%→0% 2026-09-19):** shapes HOW the white-noise riser builds (higher = later bloom, same directional sense as Tension); does not change noise level — see [parameter-spec.md](parameter-spec.md)'s `noiseTension` entry and `plugins/DROP/CHANGELOG.md` |

## UI Concept

> **FINALIZED 2026-09-14 — design locked as mockup v10 "Light Neumorphic / White Marble" (chosen by the user over v9 "Dark Iridescent"; both remain in `.ideas/mockups/`). The mockup files (`v10-ui.yaml`, `v10-ui-test.html`) are the source of truth for all UI details; this section is a summary. The spec's technical rules hold: UI never executes DSP, no DSP/locks/allocations in paint, lock-free audio→GUI communication, processor owns state.**

**Layout:** Two pages (MAIN / ADVANCED), navigated by a header pill; base 1000×560, resizable (min 800×450, max 1400×790) with proportional em-based scaling.
- **MAIN:** Intensity + Dry/Wet (large neumorphic knobs), audio-reactive HD soap-bubble centerpiece, phase chip with LED, Build Length step circles, black LCD Style selector (`< CLEAN EDM >`), DROP trigger pill, progress strip with drop marker.
- **ADVANCED:** Manual/MIDI/Auto trigger-mode pill, Tension/Chaos dot-matrix readouts, tension-curve display, LED-ring Impact hero knob (centered on the page axis), 2×5 grid of the 10 module macros, Output dB readout.

**Visual Style:** Bright HD white-marble body (photographic texture, bundled asset `v10-marble2-texture-hd.jpg`; Calacatta and raw-stone slices on disk as alternates), soft neumorphic 3D controls (extruded white domes in recessed wells, dual shadows), warm orange accent for arcs/LEDs/dot-matrix text, black inset LCD screens for readouts. Text directly on marble carries a subtle white halo for legibility (contrast verified numerically against the texture).

**Key Elements:**
- **(Amended 2026-09-14: the soap-bubble visualization was REMOVED at user direction — see mockups/v10-ui.yaml `bubble_removal`.)** The Main page centers on the hero DROP trigger; the progress strip carries the visualization contract (fill tracks build progress; fill brightness/glow scales with tension × input level via the AudioAnalyzer ring-buffer feed) alongside the phase chip + LED.
- DROP trigger: raised neumorphic pill at hero scale, press-in physics, orange arm glow; full-groove orange progress bar beneath.
- 21 APVTS parameters bound (incl. `digital`; module macros default 100%; `tail` added 2026-09-16 as a POST-1.0 amendment, small macro-style knob below the tension-curve graph as of 2026-09-17; `noiseTension` added 2026-09-17, dark-LCD mini-screen chip in the Advanced page's left column below Tension/Chaos).

## Use Cases

- Producer inserts DROP on a bus, picks 4-bar build, presses DROP, gets a convincing build → vacuum → clean impact synchronized to tempo.
- Vocal builds (filter/delay/pitch/reverb/width, no aggressive destruction), drum builds (gate/stutter/filter/saturation), bass builds (sub-protected filter/distortion/pitch), full-mix transitions (conservative, transient/low-end protected) — all source-aware via lightweight AudioAnalyzer.
- MIDI-triggered or automation-triggered drops for live performance and arrangement; deterministic offline renders for final bounces.

## Inspirations

- EDM/Big Room/Techno/Trap/DnB transition and riser workflows (16 presets spanning Clean EDM → Destroy → Reverse Energy)
- Sibling plugin in this repo: Transitionist (transition chain) — DROP differs by coordinated staging, IntensityBudget, and the Vacuum/Impact philosophy

## Technical Notes

Key subsystems (full detail in [implementation-spec.md](implementation-spec.md)):

- **MusicalClock:** BPM/PPQ/time-sig/bar/beat → exact sample positions; divisions 1/1–1/32 + triplets; handles tempo/SR changes, looping, transport, offline rendering; never wall-clock.
- **TransitionEngine:** normalized 0–1 progress; Linear/Exponential/S-Curve/LateAcceleration/Custom curves; serializable custom curve.
- **State machine:** Idle → Arming → Building → Peak → Impact → Recovery; deterministic, sample-accurate, retrigger-safe.
- **AudioAnalyzer:** RMS, peak, transient energy, spectral centroid, band energies, stereo correlation — lightweight, no ML.
- **IntensityBudget:** central 1.0 budget distributed across 10 modules by DropStyle × progress; deterministic.
- **DSP modules:** Filter (HP/LP/BP/dual), Width (M/S, low-freq protected), Pitch (granular/phase-vocoder, musical semitone steps), Gate (tempo-synced patterns), Stutter (preallocated buffers, 1/4–1/64), Delay (interpolated, feedback-safe), Reverb (low-CPU), Saturation (5 modes, selective 2x/4x oversampling), Digital (bit/SR crush accents), Noise (risers), Vacuum (1/64–1/16 click-free collapse), ImpactEngine (source-transient-first, optional 40–80 Hz sub, expansion, short tail).
- **Chaos:** std::mt19937, seed saved in state, musically-quantized randomization (e.g. 1/8 delay, +5 semitones), fully reproducible.
- **Guarantees:** RT-safe processBlock (no alloc/locks/IO), sample-accurate events via block segmentation, transient preservation, <100 Hz low-frequency protection, denormal/NaN safety, zero-latency MVP preferred, deterministic offline rendering, 44.1–192 kHz, mono+stereo.
- **Presets:** 16 minimum, each with genuinely different transition *behavior* (via DropStyle data), not just parameter values.
- **Stack:** C++20, JUCE, CMake; VST3 + Standalone; data-driven DropStyle structs, no giant switch statements.

## Next Steps

- [ ] Stage 0 planning (`/plan DROP`) — architecture.md + plan.md from the implementation spec
- [ ] Create UI mockup (`/dream DROP` → mockup option)
- [ ] Implementation (`/implement DROP`) — 10-phase plan defined in spec
