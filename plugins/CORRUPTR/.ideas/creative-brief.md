# CORRUPTR - Creative Brief

## Overview

**Type:** Effect
**Core Concept:** A multi-type distortion/glitch "playground" — a modular, rhythmic, macro-driven destruction engine that scales from subtle saturation to total digital collapse.
**Status:** 💡 Ideated
**Created:** 2026-09-01

## Vision

CORRUPTR is not a single distortion — it's an animated, playable distortion *system*. A modular signal chain (Pre-EQ → Distortion → Bitcrush/SRR → Wavefolder/Digital FX → Glitch → Rhythmic Gate/Stutter → Filter → Pitch/Frequency FX → Stereo → Post-EQ → Mix → Output/Limiter) where every module is independently bypassable and new processors can be added later without re-architecting.

The identity is five pillars working together rather than in isolation: **Distortion + Glitch + Rhythm + Modulation + Randomness + Macros.** A user should be able to take clean bass, drums, vocals, synths, or guitar and turn it into evolving, rhythmic, distorted, glitchy, experimental audio — useful across the full range from subtle warmth to extreme destruction.

**Sonic/UX lineage:** iZotope Trash / Soundtoys Decapitator — musical, warm-to-aggressive distortion character with a strong preset culture, rather than a purely clinical/technical tool.

**Primary lens:** Studio sound-design focus. Deep tweaking, precise automation, and non-real-time experimentation are the primary use pattern — though live-performable elements (macros, XY pad, Performance Mode) are still part of the MVP identity, not bolted on later.

## Signal Chain

```
INPUT → PRE-EQ → DISTORTION → BITCRUSH/SAMPLE-RATE REDUCTION →
WAVEFOLDER/DIGITAL FX → GLITCH → RHYTHMIC GATE/STUTTER → FILTER →
PITCH/FREQUENCY FX → STEREO → POST-EQ → MIX → OUTPUT/LIMITER
```

Every module independently bypassable. DSP architecture must support adding new processors later without restructuring the chain.

## Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| Drive (master) | 0-40 dB | TBD (engineer-facing scale) | Technical gain-staging control feeding waveshaper stages |
| Mix (master) | 0-200% | 100% | Dry/wet blend; >100% allows wet signal to dominate/overdrive the blend (parallel-style boost) |
| Bit Depth | TBD | TBD | Bitcrusher resolution reduction |
| Sample Rate Reduction | TBD | TBD | SRR amount |
| Fold | TBD | TBD | Wavefolder fold amount |
| Feedback (distortion feedback path) | TBD, soft-clamped | TBD | Conservative auto-limiting feedback gain — cannot runaway even at max setting |
| Filter Cutoff | 20 Hz - 20 kHz (assumed standard) | TBD | Pre/post filter cutoff |
| Glitch Probability | 0-100% | TBD | Per-event probability for glitch engine |
| Chaos (global) | 0-100% | 0% (deterministic) | 0% = fully deterministic, 100% = highly unpredictable but musically constrained |
| XY Pad X/Y | Macro-mapped | X=Damage, Y=Glitch | Smooth with inertia/glide response (not instant/snappy) — feels like a performance instrument, avoids zipper noise |
| Sequencer steps | 16 or 32 | 16 | Host-synced only in MVP (no free-run internal clock) |
| Quality/Oversampling | ECO / NORMAL (2x) / HIGH (4x) / EXTREME (8x) / AUTO | User-selectable, not fixed | Exposed as a direct user control rather than a hidden default — user asked to be able to choose explicitly |
| Output Limiter Style | Transparent / Colored | User-selectable toggle | Transparent = pure safety net; Colored = adds gentle saturation/character on limiting |

*(Full parameter set per distortion type, glitch mode, sequencer lane, and mod matrix slot to be finalized in Stage 0 DSP research — this table captures MVP-critical decisions made during ideation, not the full spec.)*

## Distortion Types (Full Vision)

Analog Saturation, Soft Clip, Hard Clip, Tube, Diode, Transistor, Fuzz, Octave Fuzz, Wavefolder, Wavefold + Clip, Rectifier, Asymmetric Distortion, Digital Distortion, Bitcrusher, Sample Rate Reducer, XOR/AND/OR digital distortion, Ring-Mod Distortion, Feedback Distortion, Foldback Distortion, Quantizer, plus experimental/custom waveshaping. Common controls: Drive, Tone, Bias, Threshold, Fold, Feedback, Mix.

### MVP Distortion Algorithm Set (expanded from original 6-algorithm plan)

Saturation, Soft Clip, Hard Clip, Tube, Diode, Fuzz, Digital Distortion, Bitcrusher, Sample Rate Reducer, Wavefolder, Ring-Mod, Foldback — **12 algorithms**, not 6. User explicitly chose to combine all three proposed MVP algorithm sets rather than picking one. This is a real scope increase over the original "6 distortion algorithm MVP" framing in the raw brief — flag during Stage 0 planning for complexity/timeline impact.

## Multiband Distortion (Post-MVP)

2/3/4-band processing, each band with its own algorithm, Drive, Mix, Tone, Filter, modulation. Example: SUB = saturation, LOW-MID = hard clip, MID = wavefolder, HIGH = bitcrusher.

## Parallel Distortion (Post-MVP)

Multiple distortion lanes in parallel, each with algorithm, drive, tone, mix, filter, pan. Example: clean + saturation + fuzz + bitcrusher + wavefolder.

## Glitch Engine

Real-time glitch/buffer system with circular audio memory. Modes: Stutter, Repeat, Reverse, Buffer Repeat, Freeze, Slice, Random Slice, Micro Loop, Granular Repeat, Tape Stop, Tape Start, Retrigger, Silence, Noise Burst, Bitcrush Burst, Pitch Jump, Random Repeat. Buffer lengths in musical divisions: 1/64 through 4 bars.

## Rhythmic Sequencer

Host-synchronized 16/32-step sequencer (MVP: host-synced only, no internal free-run clock). Steps modulate Drive, Mix, Filter Cutoff, Bit Depth, Sample Rate, Glitch Probability, Volume, Pan, Pitch, Gate. Timing: 1/4, 1/8, 1/16, 1/32, triplets. Pattern controls: Random, Mutate, Reverse, Mirror, Shift, Half, Double, Syncopate, Clear, Humanize.

## Probability / Chaos

Every rhythmic event: 0-100% probability. Global CHAOS control (0% deterministic → 100% highly unpredictable, but must stay musically useful). Deterministic random seeds so presets behave consistently across sessions.

## Euclidean Rhythms (Post-MVP)

Steps, Pulses, Rotation. Generated rhythms can drive distortion, gates, glitches, filter movement.

## Modulation

4+ LFOs — sine, triangle, saw, reverse saw, square, sample & hold, random, smooth random, random-walk. Free Hz and tempo sync. Additional sources: envelope follower, audio level, step sequencer, random, MIDI/automation.

**MVP routing decision:** All 4 LFOs are fully free — routed through the mod matrix to any destination (no hardwired LFO-to-filter shortcut). Matches the full modulation-matrix vision from day one.

## Modulation Matrix

Sources: LFO1-4, Envelope, Audio Level, Sequencer, Random, MIDI CC, Macro Controls. Destinations: Drive, Mix, Filter, Bit Depth, Sample Rate, Fold, Feedback, Glitch Probability, Glitch Size, Pitch, Pan, Width, etc. Each assignment: Amount, Polarity, optional Curve.

## Randomization

SAFE RANDOM, CREATIVE RANDOM, EXTREME RANDOM — intelligent, not uniform, randomization. Plus Random Pattern, Mutate Pattern, Chaos, Syncopate, controlled parameter randomization.

## Filters

Pre/post: LP, HP, BP, Notch, Comb, Resonant LP/HP. Cutoff, Resonance, Drive, modulation.

## Pitch / Frequency FX

Octave up/down, semitone jumps, random pitch, rhythmic pitch stutter, granular pitch, frequency shifter, ring modulation — integrated with the rhythmic engine.

## Stereo

Independent L/R processing, stereo offset, stereo spread, channel linking, phase inversion, optional Mid/Side. Example: left = bitcrusher, right = wavefolder.

## Feedback Routing (MVP — confirmed in, not deferred)

Controlled feedback routing: DISTORTION → FILTER → DELAY → DISTORTION. Controls: Feedback, Damping, Filter. **Safety default: conservative auto-limit** — feedback gain is soft-clamped before it can runaway, prioritizing stability over letting users self-oscillate into chaos unassisted. Must protect against NaN/Inf at all settings.

## Micro Delay / Comb

0.1-50 ms delay for metallic textures, comb filtering, stereo widening, phase destruction. Time, Feedback, Modulation, Mix.

## Tape / Mechanical FX

Tape Stop, Tape Start, Wow, Flutter, Speed Modulation, Dropout, Mechanical Noise — rhythmically triggerable.

## Noise

White, Pink, Digital, Static, Vinyl-like, Glitch Noise — modulatable by rhythm, envelope, LFO, or random.

## Macros (MVP — confirmed in, not deferred)

8 macro controls: DAMAGE, CRUSH, GLITCH, CHAOS, RHYTHM, MOVEMENT, WIDTH, MIX — each controlling multiple parameters simultaneously (e.g., DAMAGE → Drive + Clip + Fold + Feedback; GLITCH → glitch probability + buffer repetition + sample-rate reduction + pitch variation). Included in MVP because the XY pad's default axes (X=Damage, Y=Glitch) depend on them — building the macro layer was unavoidable regardless, so all 8 ship together rather than just the 2 the XY pad strictly needs.

## XY Pad

Large, configurable. Default X = Damage, Y = Glitch. Either axis can control any macro or parameter. **Smoothing/inertia confirmed for MVP** — the pad glides toward target values rather than responding instantly, giving it a performance-instrument feel and avoiding zipper noise. Optional MIDI control.

## Performance Mode (MVP — confirmed in, not deferred)

Large live-performance controls: KILL, GLITCH, DESTROY, FREEZE, REVERSE, STUTTER, CHAOS. MIDI-triggerable, momentary/toggle behavior. Included in MVP as core to the "playground" identity rather than deferred.

## Scenes (Post-MVP)

Four scenes A/B/C/D containing complete processing states, switchable via MIDI, sequencer, or probability. Example: A=Clean, B=Saturated, C=Destroyed, D=Glitched.

## Preset Morphing (Post-MVP)

Continuous morphing between Preset A and B — interpolates DSP parameters, modulation, rhythm, and scenes.

## Presets

Professional preset browser. **MVP target: ~15-20 presets** — one strong example per category (Distortion, Saturation, Bass, Fuzz, Glitch, Rhythmic, Experimental, Industrial, EDM, DnB, Techno, IDM, Vocals, Drums, Extreme), rather than deep coverage per category. Example names: Warm Bass, Crunchy Synth, Broken Drum, Digital Snare, Neuro Mutation, Industrial Machine, Rhythmic Crusher, 16th Glitch, Randomized Bass, Frozen Buffer, Arcade Destroyer, Broken Console, Alien Transmission, Machine Error, Total Collapse.

## UI Concept

**Layout:** Modern dark experimental professional interface. Top bar: preset browser, preset name, save, random, undo/redo, A/B. Main area: large distortion visualization/oscilloscope/spectrum. Large DRIVE control and algorithm selector front and center. Dedicated sections for GLITCH, RHYTHM, MODULATION, FILTER, OUTPUT.

**Visual Style:** Dark, experimental, professional — not a toy aesthetic despite the playful "playground" concept.

**Key Elements:**
- Waveform, spectrum, input/output meters, rhythmic animation, modulation visualization
- Modulated parameters show modulation ranges around their knobs
- Large XY pad (Damage/Glitch default axes) with glide/inertia response
- **Distortion Graph** — a visual modular routing graph (INPUT → SATURATION → WAVESHAPER → FILTER → BITCRUSH → GLITCH → PITCH → OUTPUT) with connectable modulation links between nodes. Flagged in the raw brief as a potential major visual-identity feature — worth prioritizing in UI mockup exploration since it differentiates CORRUPTR from typical distortion-plugin UIs.

## Use Cases

- Subtle saturation/warmth on bass, vocals, synths, guitars in otherwise clean mixes
- Aggressive bass sound design and EDM/techno/DnB/industrial/IDM production
- Rhythmic, sequenced distortion/glitch effects synced to host tempo
- Live-performable destruction via macros, XY pad, and Performance Mode buttons
- Total digital/extreme sound destruction for experimental sound design

## Inspirations

- iZotope Trash — musical, preset-driven distortion character
- Soundtoys Decapitator — warm-to-aggressive analog-flavored distortion lineage
- (Explicitly not modeled directly on Ableton Redux/Bitwig glitch tools or Kilohearts/Serum wavefolding — those were considered and declined in favor of the Trash/Decapitator lineage)

## Technical Notes

- **DSP architecture:** Modular signal chain, every module independently bypassable, designed for future processor additions without restructuring.
- **CPU budget target:** Moderate — ~3-5% per instance on a modern CPU. Allows HIGH quality mode as a reasonable default without punishing large sessions.
- **Quality/oversampling:** ECO/NORMAL/HIGH/EXTREME/AUTO modes, exposed as a direct user-facing control (not fixed/hidden). Oversampling applied around nonlinear stages to control aliasing.
- **Feedback safety:** Conservative auto-limiting on the DISTORTION→FILTER→DELAY→DISTORTION path — soft-clamps feedback gain before runaway, protects against NaN/Inf at all settings.
- **Output limiter:** User-selectable Transparent (pure safety net) vs Colored (adds character) modes.
- **Real-time safety:** No allocation, filesystem access, blocking ops, or unnecessary locks on the audio thread. UI/preset operations communicate with DSP via real-time-safe mechanisms.
- **State:** Every parameter, algorithm choice, sequencer pattern, modulation routing, macro mapping, scene state (post-MVP), preset state, and relevant randomization seed must be saved.
- **Testing:** Automated tests per DSP algorithm, parameter boundaries, silence, extreme input, mono/stereo, sample-rate changes (44.1/48/88.2/96/192 kHz), block-size changes, automation, preset load/restore, feedback stability, NaN/Inf protection, denormal handling.
- **MVP scope note:** MVP is already larger than the original raw-brief framing — macros (all 8), Performance Mode, and feedback routing are all confirmed IN for MVP alongside the original core list (I/O, 12 distortion algorithms instead of 6, bitcrusher, filter, mix, limiter, 16-step sequencer, glitch/stutter, 4 LFOs, mod matrix, ~15-20 presets, randomization, XY pad w/ inertia, VST3 state saving, user-selectable oversampling, meters). Multiband, parallel distortion, Euclidean rhythms, scenes, preset morphing, frequency shifter, pitch destruction, granular processing, and advanced glitch modes remain post-MVP. Flag the expanded scope during Stage 0 planning for realistic complexity/timeline assessment.

## Next Steps

- [ ] Create UI mockup (`/dream CORRUPTR` → option 3) — the Distortion Graph routing view is a strong candidate to mock up first given its visual-identity potential
- [ ] Start implementation (`/implement CORRUPTR`)
