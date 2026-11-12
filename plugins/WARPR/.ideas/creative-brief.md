# WARPR - Creative Brief

> **Working title.** User will choose the final name later — rename the `plugins/WARPR/` folder and this brief before `/plan` or `/implement`.

## Overview

**Type:** Synth (Sample-Based Instrument)
**Core Concept:** A sample-based instrument that feeds any loaded sample through an exceptionally deep wave-warp engine — bending, folding, distortion, phase distortion, and FM/PM/AM/RM — with an editable A/B-morphing waveshaper, an extensive multimode filter, drag-drop modulation, and a strict monochrome interface.
**Status:** 💡 Ideated
**Created:** 2026-11-11

## Vision

A hybrid between a sampler and an advanced wavetable synthesizer: load a sample, play it polyphonically, and mangle its waveform with the kind of warp-mode depth Serum gives its oscillators — but applied to arbitrary sample material. Conceptual inspiration from Serum's warp modes and filter architecture only; original product, interface, code, and identity (no copied assets or branding).

"Organ" in the original pitch meant *instrument* generally, not church organ — this is a general-purpose sample-mangling instrument.

Priorities, in order: sound quality, waveform manipulation depth, immediate control, elegant monochrome UI.

**Scope decision:** ALL listed warp modes (~60) and filter types (~34) ship in v1.0. No trimmed "curated core" — the exhaustive mode list *is* the product. Every menu entry must have a real, independent DSP implementation; no placeholder options, ever.

## Signal Architecture

```
SAMPLE LOADER
→ SAMPLE PLAYBACK ENGINE (per-voice, polyphonic; Classic or Granular mode)
→ WAVE WARP ENGINE            [enable + dry/wet]
→ WAVESHAPER / DISTORTION     [enable + dry/wet]
→ MULTIMODE FILTER            [enable + wet/dry per original spec]
→ AMPLIFIER ENVELOPE
→ OUTPUT
```

Each processing stage gets an **enable toggle and a dry/wet mix knob**. DSP built as independent, reusable C++ classes, fully separated from GUI.

## 1. Sample Engine

- WAV + AIFF loading via `juce::AudioFormatManager`; drag-and-drop; sample preview
- Waveform display; root note; MIDI pitch tracking; fine tune; octave/semitone transpose
- Sample start/end positions; loop start/end editing; forward loop, ping-pong loop, one-shot; loop crossfade
- Playback direction; playback speed; pitch tracking toggle
- Velocity sensitivity; stereo + mono support; sample gain; start-position randomization
- All decoding/file I/O off the audio thread; no allocation/locks/blocking in callbacks
- Polyphonic playback with independent per-voice state; high-quality interpolation for pitched playback
- **Ships empty** of factory samples for now (added later); include one built-in default single-cycle wave so it makes sound immediately on insert
- **Sample persistence: embed sample audio in preset/plugin state** so presets and DAW projects always reopen intact

### Granular Mode (Granulator-III-inspired, original implementation)

The sample section has a **Classic / Granular playback mode toggle**. Granular replaces normal playback (per preset) and feeds the **same warp → shaper → filter chain** — granulate, then mangle. Per-voice, polyphonic, velocity-sensitive like classic mode.

**Grain modes:** CLASSIC (two overlapping grains per channel) / LOOP (crossfading playback with random position) / CLOUD (up to ~20 unsynced grains per channel — dense textures).

**Controls (v1.0):** POSITION (%), SCAN (ramp speed from note-on) + DIST (scan end relative to position) with SCAN-LOOP and SCAN-KEY toggles, SIZE (2 ms–2 s), SHAPE (grain-envelope morph; crossfade length in Loop; fixed in Cloud), VAR (per-grain variation amount: position/size/pitch/volume sub-amounts), SPREAD (stereo grain detune), plus a mode-dependent slot — disabled in Classic, REV % (reverse chance) in Loop, DENSITY in Cloud.

**Behavior:** POSITION is a first-class mod destination (drag an LFO onto it). Grain size changes apply after the current grain completes (click-free). All grain scheduling from preallocated pools; no audio-thread allocation.

## 2. Wave Warp Engine (signature feature #1)

Selectable WARP MODE menu + shared WARP AMOUNT control (plus mode-specific secondary parameters). Every mode is a real, independent implementation.

**Wave Bending:** Bend Positive, Bend Negative, Bend Symmetric, Bend Asymmetric, Asym Positive, Asym Negative, Asym Bidirectional, PWM, Flip, Mirror, Wave Skew, Wave Stretch, Wave Compress, Wave Expand, Variable Duty Cycle

**Wavefolding:** Linear Fold, Multi Fold, Sine Fold, Triangle Fold, Soft Fold, Hard Fold, Symmetric Fold, Asymmetric Fold, Dynamic Fold, Chebyshev Fold

**Distortion:** Hard Clip, Soft Clip, Tanh, Arctangent, Tube Saturation, Diode 1, Diode 2, Cubic, Sinusoidal, Exponential, Asymmetric, Polynomial, Chebyshev Harmonic, Full Wave Rectification, Half Wave Rectification, Soft Rectification, Bitcrush, Sample Rate Reduction, Quantization

**Phase & Time Warp:** Phase Distortion, Phase Stretch, Phase Compression, Phase Mirror, Phase Skew, Phase Quantization, Phase Remapping, Windowed Sync, Hard Sync, Soft Sync

**Modulation:** FM, PM, AM, RM, Self FM, Cross Modulation

**Internal modulation oscillator:** second oscillator usable as FM/PM/AM/RM source without contributing directly to audible output. **The loaded sample itself can also act as modulator** (self-FM / cross-modulation focus); basic shapes are secondary.

Parameter smoothing throughout; no clicks on algorithm switch or parameter change.

## 3. Custom Waveshaper Editor (signature feature #2)

Large interactive transfer-function editor (input amplitude → output amplitude):

- Add/remove/drag control points; linear, smooth, and cubic interpolation
- Symmetric and asymmetric modes; separate positive/negative curve control
- **Curve A + Curve B with a Morph A/B knob**
- Reset, normalize, invert
- Preset curves: soft saturation, hard clip, tube, diode, fold, sine, triangle, exponential, custom
- Displays the actual transfer function, updates immediately
- GUI→audio transfer via precomputed lookup tables built off the audio thread, interpolated at playback; no audio-thread allocation

## 4. Filter Engine

Dedicated multimode filter with an extensive type selector. Filter-specific DSP classes (not one forced algorithm); `juce::dsp::StateVariableTPTFilter` as foundation where suitable, independent topologies elsewhere.

**Standard:** LP 6/12/18/24/36/48 dB, HP 6/12/18/24/36/48 dB, BP 6/12/24 dB, Notch, Variable Notch, Peak, Low Shelf, High Shelf

**Special:** Comb+, Comb−, All Pass, Serial All Pass, Formant, Vowel, Morphing Formant, Phaser, Resonant LP, Resonant HP, Distortion LP, Scream, Sample & Hold, Ring Mod Filter

**Controls:** enable, type (with prev/next stepping), cutoff, resonance, drive, **morph** (type-dependent character), key tracking, per-stage mix. (Filter envelope and output gain were cut during UI iteration; cutoff modulates via the LFO system.) The filter panel is led by a **full-width interactive response display**: filled curve, cutoff guide line, and a draggable node (x = cutoff, y = resonance) that stays in sync with the knobs. Smoothed cutoff/resonance modulation; click-free, stable.

## 5. Envelope

- **ONE amp ADSR** (A/D/S/R, dedicated knob each) — per-voice volume. The filter envelope was cut during UI iteration; cutoff is modulated via the LFO/mod system instead.
- The envelope is **drawn directly on the sample waveform display** with draggable handles (attack peak, decay→sustain corner, sustain end, extendable release end); handles and knobs stay bidirectionally in sync. All stage times shown in ms.
- Correct note-on/off, retrigger, voice stealing, release (per-voice).

## 6. Modulation System (drawable LFOs + drag-drop assignment)

- **Up to 10 independent LFOs** (3 visible by default + add), each with a **drawable grid editor**: add/drag/delete points, per-segment curve-tension handles, independent X (2–32/OFF) and Y (2–16/OFF) grid snap, cell drawing tools (point / flat / rise / fall), Shift-drag step sequencing.
- **Exact analytic shape presets** (sine, triangle, saws, square, pump, steps 4/8/16, swing, S&H noise, drift); clicking a preset curve drops a single point that bends it locally — no forced conversion.
- Modes FREE / RETRIG / ENV / MONO; playback forward / reverse / ping-pong; free-Hz and host-synced rates; RATE / PHASE / SMOOTH.
- **Drag-drop assignment:** drag an LFO number chip onto any knob; the knob shows a thin gray depth rim (invisible until assigned) + source badge; drag rim/badge for depth, double-click badge to remove.
- Engine: sample-accurate, polyphonic where appropriate, DAW-transport synced; all shapes/grids/modes/assignments persist in presets and host state.
- Additional mod sources: velocity, mod wheel, note number.

## 7. Polyphony & MIDI

- Polyphonic playback, **default 16 voices, max 32**, configurable
- Proper voice allocation and stealing, smooth release, no stuck notes
- Pitch bend with **selectable ±2/±12/±24 semitone range**; mod wheel; velocity response
- Mono/legato mode with **glide (portamento time knob, works in mono and poly)**
- **Full unison: up to 8 unison voices per note with detune and stereo spread controls**

## 8. Performance & Quality

- Configurable 1x/2x/4x/8x oversampling (16x optional if performance allows) via `juce::dsp::Oversampling`, wrapped around nonlinear stages
- Anti-aliasing attention especially for hard clip, wavefolding, FM, phase distortion
- High-quality interpolation; parameter smoothing; DC offset protection; output gain protection; denormal protection; stable filters; correct stereo
- No audio-thread allocation/mutexes/blocking; correct `prepareToPlay`/`releaseResources`/sample-rate handling

## UI Concept

(Volunteered by user — captured verbatim in spirit, to be developed in ui-mockup phase.)

**Visual Style:** Strict black and white. Pure/near-black background, white text, light-grey secondary text, dark-grey control backgrounds, white waveform lines, thin borders, minimal clutter. No color, no gradients, no colored LEDs, no skeuomorphism, no decorative graphics. Professional modern sound-design instrument feel.

**Layout (FINAL — locked by mockup v9, the source of truth):**
- **Header (88px):** WARPR title, preset name + prev/next, SAVE/LOAD, settings, MASTER knob + stereo meter
- **Left SOURCE panel (~500px, merged):** sample waveform screen with draggable amp-ADSR overlay, draggable START/END lines with dimmed out-regions and sweeping playhead; START/END knobs, TUNE group (root stepper, semi, fine), LOOP group (mode select, direction); CLASSIC/GRANULAR toggle swapping in the Granulator-III-style grain page; ENVELOPE knob row (single ADSR); WARP|SHAPER tabbed slot (warp: 60-mode menu, WARP knob, A/B, mod osc, OS, mix; shaper: transfer curve, A/B, morph, presets, reset/normalize, mix)
- **Right stage:** large ultra-thin-line contour organism (audio-reactive in production: low band = breathing, high band = shimmer, transients = swells, silence relaxes) over faint telemetry
- **Under stage:** LFO panel (tabs 1–3/+, shape-preset menu, direction, HZ/SYNC; mode rail FREE/RETRIG/ENV/MONO left of the large drawable grid editor, tool rail PT/FLAT/UP/DN right; footer with X/Y grid snap and RATE/PHASE/SMOOTH) beside FILTER panel (type prev/next menu, full-width filled response display with draggable node, CUTOFF/RES/DRIVE/MORPH/KEY/MIX row)
- **Bottom bar (84px):** POLY/MONO/LEGATO, voices, unison count + DETUNE/SPREAD, GLIDE, bend range
- **Knobs:** inset-well caps with tick rings, white needles, Serum-orientation sweep (0% bottom-left → 100% bottom-right), value chips; modulation = thin gray rim arc + numbered badge, hidden until assigned

**Knob behavior:** value readout, parameter name, white indicator, fine mouse control, double-click reset, mouse-wheel, host automation, tooltips.

**Waveform visualization:** real-time displays of original sample, post-warp, and post-distortion waveforms with zoom and scrolling; must reflect actual DSP output (lock-free/buffered transfer from audio thread; no heavy work in `paint()`).

Scales correctly, sensible minimum size, no scrolling at default window size.

## Use Cases

- Aggressive sound design — growls, basses, metallic textures with the warp engine pushed hard
- Keys/organs reimagined — playable keyboard instrument with movement and grit
- Pads and drones — looped sustained textures with LFO-morphed warp
- Leads — cutting mono/poly leads using sync, fold, and unison

## Inspirations

- Serum — oscillator warp modes and filter architecture (concepts only; original identity)
- Classic wavefolders (Buchla-style) and phase-distortion synthesis (Casio CZ lineage)

## Technical Notes

- C++17+, JUCE, CMake; VST3 + Standalone, AU on macOS; Windows + macOS
- `AudioProcessor` / `Synthesiser` / `SynthesiserVoice` / `SynthesiserSound` / APVTS with stable parameter IDs; all parameters host-automatable; parameter attachments for GUI
- Modular `Source/DSP/` + `Source/GUI/` architecture (SampleEngine, WaveWarpEngine, WaveFolder, WaveShaper, DistortionEngine, ModulationEngine, FilterEngine, EnvelopeEngine, OversamplingEngine, Voice, …)
- Presets: save/load, factory preset support (content later), user preset directory, full state + embedded-sample restoration in host projects
- Automated tests for every distortion/fold algorithm, waveshaper interpolation and symmetry, phase remapping, sample playback and loop boundaries, ADSR, filter response/stability, oversampling, stereo, persistence, preset loading, MIDI handling; NaN/inf/DC/discontinuity checks
- Original milestone plan (1: project+playback+ADSR+basic GUI → 2: visualization+bending → 3: folds/distortion+oversampling → 4: shaper editor → 5: filters+filter ADSR → 6: FM/sync/remaining warps → 7: presets+tests → 8: optimization/release) maps onto the PFS staged workflow; preserve its per-milestone verify-compile-test discipline
- Implementation rules: no fake controls, no placeholder menu entries, no visual-only "processing", no DSP in GUI classes, no silent omission of hard algorithms — flag anything unimplemented explicitly

## Next Steps

- [ ] Choose final plugin name (rename folder + this brief)
- [ ] Create UI mockup (`/dream WARPR` → option 3)
- [ ] Start planning/implementation (`/plan WARPR` → `/implement WARPR`)
