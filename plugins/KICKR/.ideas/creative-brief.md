# KICKR - Creative Brief

## Overview

**Type:** Synth (Instrument) — dedicated kick-drum design tool
**Core Concept:** MIDI-triggered kick instrument with **two blendable sources** — (1) an algorithmic synthesis engine (body + pitch envelope + transient/click + sub + tail + noise) and (2) a sample-playback layer fed from the user's own bank of recorded kicks — summed and shaped by a morphing distortion, tone, stereo and safety limiter, with a real-time waveform/spectrum analyzer and a large factory preset library.
**Status:** 💡 Ideated
**Created:** 2026-08-28
**Revised:** 2026-08-28 — added the sample-playback engine (see *Sample Engine*); reversed the "no samples" non-goal.
**Working title rationale:** "2" is a product/brand generation marker. No "Kick Designer 1" exists in this repo; **KICKR supersedes the stalled `MinimalKick` plugin** (see Relationship to Existing Plugins).

## Vision

KICKR is a focused, commercial-grade kick-generation instrument — not a general synthesizer. Every control exists to shape one triggered kick drum. It targets Techno, Hard Techno, Hardstyle, Hardcore, Hardcore/Gabber, EDM, House, Trap, DnB, and Industrial production, spanning clean sub-heavy club kicks through fully destroyed hard-techno distortion.

The synthesis engine builds the kick **algorithmically** from independent, individually controllable layers (body, sub, click, tail, noise). Alongside it, an optional **sample layer** plays one of the user's own recorded kicks with its own pitch, trim, envelope and lo-fi shaping. Either source can be turned off: synth-only (default), sample-only, or both blended (e.g. a recorded top-end over a synthesised sub). The combined signal is then shaped by a transient stage, a morphing multi-algorithm distortion, a simple 3-band tone control, frequency-dependent stereo widening, and a transparent safety limiter. Nonlinear stages run inside selectable oversampling (1×/2×/4×/8×).

Speed of sound design is a first-class goal: four large macro knobs (Punch, Body, Crush, Tail) produce immediately musical changes, and constrained Randomize / Mutate functions generate usable kicks rather than noise.

The interface is a premium dark modern **native JUCE** UI (custom controls + LookAndFeel), resizable, with a large real-time waveform display of the most recent kick and a low-CPU spectrum view.

## Non-Goals (explicit)

- No wavetable synthesis, no polyphonic oscillator stacks, no arpeggiator, no modulation matrix, no generic multi-effects rack.
- No full mastering EQ (tone is 3 simple bands only).
- **Sample engine:** no factory / bundled / third-party samples — the bank is *only* the user's own recordings, dropped in by them. No multisampling, velocity layers or round-robin in v1 (one sample per patch). No time-stretch, no slicing, no built-in recorder.
- Initial release is **monophonic** (fast retriggerable single voice). Polyphony is out of scope for v1.

## Relationship to Existing Plugins

- **MinimalKick** (`🚧 Stage 5`, stalled): KICKR is its successor. Reusable concepts/code (sine body oscillator, exponential pitch envelope formula `f = f0 * pow(2, env * semis/12)`, AD amp envelope with sustain=0, tanh drive) should be migrated. **Mark MinimalKick `🗑️ Archived` once KICKR reaches ✅ Working.**
- **Drum808** (`📦 Installed`): full 808 drum kit; different product (multi-voice kit vs. dedicated kick designer). No conflict.

## Signal Flow

```
MIDI Note-On (velocity)
   │
   ▼
KickEngine → KickVoice (monophonic, retriggerable, click-free)
   │
   ├─ SamplePlayer ──── one user sample: resample(tune/MIDI) → trim/reverse → AD env → HP/LP + bitcrush   [SAMPLE on/off]
   ├─ BodyOscillator ── PitchEnvelope (exp/log, high→fundamental) ── AmplitudeEnvelope (fast atk / exp decay)  ┐
   ├─ SubOscillator ─── independent decay, mono, phase-stable                                                  │
   ├─ ClickGenerator ── filtered noise burst + transient osc + impulse, 1–15 kHz, 0.1–50 ms                    ├ [SYNTH on/off]
   ├─ TailGenerator ─── body-derived or dedicated LFO tail, own length/tone/drive                              │
   └─ NoiseGenerator ── white/pink/filtered, own decay/tone   (optional, default off)                          ┘
   │
   ▼  (layer sum — sample + synth)
TransientShaper (kick-tuned attack/sustain)
   │
   ▼
Distortion (morphing: SoftClip→Tanh→Cubic→Asym→HardClip→Foldback→BitCrush)  [inside Oversampling 1/2/4/8×]
   │  Drive / Character (continuous morph) / Drive Mix
   ▼
Tone (Low / Mid / High — high-quality shelving/bell, not a mastering EQ)
   │
   ▼
Stereo (sub & lows forced mono; controlled widening of body/click/output)
   │
   ▼
OutputStage → Output Gain → Safety Limiter (transparent, transient-preserving) → Mix → Out
```

## DSP Architecture (component map)

| Component | Responsibility | JUCE building blocks |
|---|---|---|
| `KickEngine` | Owns full synthesis, sample-rate/oversampling lifecycle, parameter smoothing, analyzer taps | — |
| `KickVoice` | One triggered kick; monophonic retrigger with equal-power crossfade / fast fade to kill clicks | — |
| `SamplePlayer` | Plays one user sample: variable-rate resample (tune / MIDI-track), start/end trim, reverse, AD envelope, HP/LP filter, bit/rate reduction. Runs in the OS region (AD-10). | `juce::Interpolators`, `dsp::StateVariableTPTFilter` |
| `SampleLibrary` | Scans + watches the managed samples folder, decodes drag-dropped files, provides the ordered browsable bank. Message thread only. | `juce::AudioFormatManager`, `juce::DirectoryContentsList`, `juce::File` |
| `BodyOscillator` | Sine fundamental + optional harmonic generation + controlled saturation. Fundamental ≈ 25–150 Hz | `dsp::Oscillator` or phase accumulator |
| `PitchEnvelope` | Exponential/logarithmic drop from `pitchStart × fundamental` → fundamental over `pitchTime`; curve control. **Never lerp Hz** — work in ratio/semitone domain | custom exp env |
| `AmplitudeEnvelope` | Fast attack (0–20 ms), exponential decay (20–2000 ms), level | `juce::ADSR` (AD, sustain 0) or custom exp |
| `ClickGenerator` | Short filtered-noise + transient-osc + impulse blend; tone 1–15 kHz, time 0.1–50 ms; anti-aliased | `dsp::StateVariableTPTFilter`, band-limited noise |
| `NoiseGenerator` | White/pink/filtered noise layer with decay + tone | `juce::Random`, pink filter, SVF |
| `TailGenerator` | Sustained tail from body or dedicated LF osc; length/tone/drive; tight→long/distorted | osc + SVF + waveshaper |
| `TransientShaper` | Kick-specific attack/sustain enhancement (simple envelope-follower design) | custom |
| `Saturator` / `Waveshaper` | 7 nonlinearities with a continuous morph selector; subtle → extreme | custom transfer functions |
| `OutputStage` | Tone (3-band), stereo width, output gain, safety limiter, mix | `dsp::IIR`, `dsp::Limiter` / custom |
| `OversamplingProcessor` | Wraps nonlinear section in `dsp::Oversampling` (1/2/4/8×), correct latency reporting, safe switching | `juce::dsp::Oversampling` |
| `DSPUtils` | denormal flush, NaN/Inf guards, DC blocker, dB/gain, exp-env helpers, pink-noise filter | — |

**Hard rules:** no allocation / locks / file I/O / heavy work on the audio thread; flush denormals; guard NaN/Inf; DC-block the output; smooth every parameter; report latency = oversampling latency; safe (glitch-free) oversampling-factor switching.

## Parameters

Full APVTS specification (stable IDs, skews, units, automation flags) is finalized in `parameter-spec.md` during planning. Defaults below are **starting points to tune by ear**, not fixed requirements.

### Sample engine (added 2026-08-28)

| Group | Parameter (ID) | Range | Default | Notes |
|---|---|---|---|---|
| Sample | `synthEnable` | Off / On | **On** | Off = mute all 5 synth layers (sample-only kick) |
| Sample | `sampleEnable` | Off / On | **Off** | Off = pure synth (current behaviour). On = sample layer active |
| Sample | `sampleLevel` | 0–1 | 0.7 | sample layer gain into the sum |
| Sample | `sampleStart` | 0–1 | 0.0 | trim start (fraction of file) |
| Sample | `sampleEnd` | 0–1 | 1.0 | trim end |
| Sample | `sampleReverse` | Off / On | Off | play the trimmed region backwards |
| Sample | `sampleTune` | −24…+24 | 0 | semitones (resample) |
| Sample | `sampleFine` | −100…+100 | 0 | cents |
| Sample | `sampleMidiTrack` | Off / On | On | pitch follows MIDI note vs. fixed at the sample's native pitch |
| Sample | `sampleAttack` | 0–200 | 0 | ms — AD envelope attack |
| Sample | `sampleDecay` | 20–2000 | 800 | ms — AD envelope decay (tighten / extend the tail) |
| Sample | `sampleHP` | 20–2000 | 20 | Hz — high-pass (20 = off), sample-only, pre-sum |
| Sample | `sampleLP` | 200–20000 | 20000 | Hz — low-pass (20000 = off), sample-only, pre-sum |
| Sample | `sampleCrush` | 0–1 | 0.0 | bit-depth + sample-rate reduction (lo-fi), sample-only |

Sample **selection** is not an automatable parameter — the chosen file name is a state property (`currentSampleName`) driven by the bank browser, exactly like the preset name.

### Primary / automatable

| Group | Parameter (ID) | Range | Default | Skew | Unit |
|---|---|---|---|---|---|
| Pitch | `fundamental` | 25–150 | 55 (A1) | freq (log) | **note** (shown as A1, C#2…; Hz in tooltip) |
| Pitch | `pitchStart` | 1–10 | 4 | mild | × fundamental |
| Pitch | `pitchTime` | 5–500 | 50 | 0.4 | ms |
| Pitch | `pitchCurve` | 0–1 (lin→exp) | 0.7 | — | — |
| Body | `bodyLevel` | 0–1 | 1.0 | — | lin / dB |
| Body | `bodyDecay` | 20–2000 | 400 | 0.4 | ms |
| Body | `bodyHarmonics` | 0–1 | 0.15 | — | — |
| Sub | `subLevel` | 0–1 | 0.5 | — | lin |
| Sub | `subFreq` | 25–80 | 40 | log | Hz |
| Sub | `subDecay` | 20–2000 | 300 | 0.4 | ms |
| Click | `clickLevel` | 0–1 | 0.4 | — | lin |
| Click | `clickTone` | 1000–15000 | 4000 | 0.4 | Hz |
| Click | `clickTime` | 0.1–50 | 3 | 0.3 | ms |
| Click | `clickPitch` | 1000–15000 | 5000 | 0.4 | Hz |
| Tail | `tailLevel` | 0–1 | 0.3 | — | lin |
| Tail | `tailLength` | 20–2000 | 200 | 0.4 | ms |
| Tail | `tailTone` | 0–1 | 0.5 | — | — |
| Tail | `tailDrive` | 0–1 | 0.2 | — | — |
| Noise | `noiseLevel` | 0–1 | 0.0 | — | lin |
| Noise | `noiseDecay` | 20–500 | 60 | 0.3 | ms |
| Noise | `noiseTone` | 0–1 | 0.5 | — | — |
| Noise | `noiseType` | White / Pink / Filtered | White | — | choice |
| Distortion | `drive` | 0–1 (→ up to ~+36 dB) | 0.3 | — | — |
| Distortion | `character` | 0–1 (morph through 7 styles) | 0.0 | — | — |
| Distortion | `driveMix` | 0–1 | 1.0 | — | — |
| Tone | `low` | −12…+12 | 0 | centered | dB |
| Tone | `mid` | −12…+12 | 0 | centered | dB |
| Tone | `high` | −12…+12 | 0 | centered | dB |
| Transient | `transientAttack` | −1…+1 | 0 | centered | — |
| Transient | `transientSustain` | −1…+1 | 0 | centered | — |
| Stereo | `bodyWidth` | 0–1 | 0.0 | — | — |
| Stereo | `clickWidth` | 0–1 | 0.3 | — | — |
| Stereo | `outputWidth` | 0–1 | 0.5 | — | — |
| Output | `output` | −24…+12 | 0 | — | dB |
| Output | `limiter` | Off / On | On | — | bool |
| Output | `mix` | 0–1 | 1.0 | — | — |
| Global | `oversampling` | 1× / 2× / 4× / 8× | 4× (confirm after CPU profiling) | — | choice |

### Tuning

| Parameter (ID) | Range | Default | Notes |
|---|---|---|---|
| `tuneMode` | MIDI Pitch / Fixed Frequency | MIDI Pitch | Fixed mode locks fundamental regardless of incoming note |
| `tune` | −24…+24 | 0 | semitones |
| `fineTune` | −100…+100 | 0 | cents |

### Velocity (sensible, minimal)

| Parameter (ID) | Range | Default | Notes |
|---|---|---|---|
| `velToLevel` | 0–1 | 0.5 | velocity → amplitude |
| `velToClick` | 0–1 | 0.3 | velocity → click level |
| `velToPitch` | 0–1 | 0.0 | velocity → pitch-start amount |

### Macros (non-destructive; layer offsets on top of base params)

| Macro | Default | Drives |
|---|---|---|
| `macroPunch` | 0.5 | transient attack + pitch-envelope attack energy (pitchStart / pitchTime) |
| `macroBody` | 0.5 | body level + body decay + fundamental |
| `macroCrush` | 0.5 | drive + character |
| `macroTail` | 0.5 | tail level + tail length + tail tone |

## Sample Engine

- **Blendable, not a mode:** `SamplePlayer` is a 6th layer summed with the synth layers before the transient stage, so the whole downstream chain (transient, drive/character, tone, stereo, limiter, the CRUSH macro, oversampling) applies to sample and synth alike. `sampleHP` / `sampleLP` / `sampleCrush` are sample-only shaping *before* the sum.
- **Three configurations** via the two enable toggles: synth-only (default), sample-only (`synthEnable` off), or both blended.
- **The bank:** the user drops WAV / AIFF / FLAC / CAF files onto the plugin window. Each is decoded and **copied** into a managed library folder — `~/Music/KICKR/Samples/` — which the plugin scans and watches. The header's browser shuttles through the bank (prev / next / list), same UX as presets. **No factory samples ship** — the bank is empty until the user fills it.
- **Loading:** files are decoded to a `juce::AudioBuffer<float>` on the message thread (kick-length; **hard cap ~5 s** at session rate to bound RAM), then handed to the audio thread via an atomic pointer swap. Missing sample on preset recall → sample layer silent + a non-modal notice; the synth layers are unaffected.
- **Playback:** one-shot on note-on, monophonic, retrigger restarts it. `sampleMidiTrack` on → note number sets playback rate relative to the sample's assumed root; off → fixed native-pitch playback (still retriggered). Velocity scales sample level (and, lightly, `sampleLP`) via `velSensitivity`.
- **Presets** reference the sample by file name; the file must be in the library folder to recall.

## Randomize / Mutate

- **Randomize:** constrained per-parameter musical ranges (genre-aware bounds), correlated where needed (e.g. shorter decay ↔ tighter tail). Target: the large majority of results are usable kicks.
- **Mutate:** small bounded perturbations (±5–15%) around the current patch. Preserves character; explores neighborhood.
- Both are undoable; **neither touches** `oversampling`, `limiter`, `output`, `mix`, the tuning group, `velSensitivity`, the macros, **or any `sample*` parameter / `synthEnable` / `sampleEnable` / the sample selection** (the sample layer is the user's material — Randomize shapes the synth, not the sample).

## UI Concept

**Framework:** native JUCE — custom `Knob`, `WaveformDisplay`, `SpectrumDisplay`, `EnvelopeDisplay`, `PresetBrowser` components + shared `LookAndFeel`. **Resizable** (constrained aspect, min size enforced).

**Style:** premium dark modern — deep neutral background, restrained accent color, crisp typography, subtle depth. No skeuomorphic hardware.

**Layout:**
- **Header:** "KICKR" wordmark · preset browser (prev/next + list) · Save · Undo/Redo · Randomize · Mutate.
- **Center:** large real-time **waveform display** of the most recent kick; toggle/overlay **spectrum** (reasonable FFT size, low CPU, never blocks audio).
- **Macro row:** four large knobs — PUNCH · BODY · CRUSH · TAIL.
- **Sample module:** drag-drop target + bank browser (prev/next/list, shows current file name) · SYNTH and SAMPLE enable toggles · sample tweak controls (Level, Start/End, Reverse, Tune/Fine, MIDI-track, Attack/Decay, HP/LP, Crush). The whole plugin window is also a drop target.
- **Section panels:** Body (the tonal core — NOTE / Fundamental + Level/Decay/Harmonics + the pitch envelope Start/Time/Curve; **no separate Pitch panel**) · Click · Tail · Sub · Noise · Drive (Drive/Character/Mix) · Tone (Low/Mid/High) · Stereo · Output.
- **Global strip:** Oversampling selector · Limiter toggle · Output gain · Mix.

**Drag-and-drop:** `juce::FileDragAndDropTarget` on the editor; dropped audio files are validated (format, length) and copied into the library folder on the message thread, then the bank re-scans and selects the new file.

**Knob behaviour (all knobs):** smooth drag, mouse-wheel, double-click reset to default, live value readout, tooltip, host-automation compatible, sensible units + precision.

**Analyzer contract:** lock-free FIFO / atomic double-buffer from audio thread to a Timer-driven repaint on the message thread. No allocation or locking in `processBlock` for visualization. Waveform captures one full kick per trigger.

## Parameter System

- `juce::AudioProcessorValueTreeState` is the single source of truth. UI never holds authoritative state.
- Every parameter: stable string ID (in `Parameters/ParameterIDs.h`), display name, default, range, appropriate skew, unit, full automation support.
- State save/restore round-trips exactly (APVTS `state` → XML → binary). Version tag for forward migration.

## Presets

- **Preset manager** with factory + **user disk presets** (user presets saved under `~/Library/Audio/Presets/PluginFreedom/KICKR/`; the sample library sits in a `Samples/` sub-folder of the same path). Factory presets baked in via `BinaryData`.
- **Factory set** (original parameter configurations only — no copyrighted material, **all synth-only / `sampleEnable` off** since no samples ship):
  Clean · House · Techno · Hard Techno · Hardstyle · Hardcore · Industrial · Sub Heavy · Short · Long · Distorted · Clicky · Punchy · Warehouse · EDM · Trap · Cinematic.
- A **user preset** may reference a sample by file name; recall requires that file to be present in the library folder (else the sample layer stays silent with a notice).
- Default patch loads on first instantiation.

## Oversampling

- `juce::dsp::Oversampling` around the nonlinear section (distortion, tail drive, body saturation).
- Factors 1× / 2× / 4× / 8×; latency reported to host = current oversampling latency; switching is glitch-free (fade or defer to next block boundary).
- Default 2× or 4× — decided after CPU profiling (Stage 17).

## MIDI

- Monophonic trigger; note-on starts a voice, note-off ignored (kick runs to completion).
- Rapid retriggering is click-free (short fade-out / crossfade of the outgoing voice).
- Velocity influences level / click / pitch per the velocity mapping params.

## Default Sound (starting point — tune by listening)

Fundamental 55 Hz · Pitch Start 4× · Pitch Time ~50 ms · Body Decay ~400 ms · moderate Click · moderate Sub · moderate Drive · Limiter On · Oversampling 2×/4×.
Goal: a convincing **punch**, not a laser sweep.

## Quality Requirements

No audio-thread allocations or locks · no clicks/pops · no runaway voices · no NaN/Inf · no excessive DC · no obvious aliasing at 44.1/48 kHz · clean behaviour at 88.2/96 kHz · low CPU · correct state restoration · correct automation · correct MIDI · correct oversampling & latency reporting · zero compiler warnings.

## Proposed Project Structure

```
plugins/KICKR/
  CMakeLists.txt
  Source/
    PluginProcessor.{h,cpp}
    PluginEditor.{h,cpp}
    DSP/  KickEngine  KickVoice  SamplePlayer  BodyOscillator  PitchEnvelope  AmplitudeEnvelope
          ClickGenerator  NoiseGenerator  TailGenerator  TransientShaper
          Saturator  Waveshaper  OutputStage  OversamplingProcessor
    Sampling/  SampleLibrary.{h,cpp}   (folder scan / watch / decode)
    Parameters/  ParameterIDs.h  ParameterLayout.h
    UI/  Knob  WaveformDisplay  SpectrumDisplay  EnvelopeDisplay  PresetBrowser  SampleBrowser  LookAndFeel
    Presets/  PresetManager.{h,cpp}   (+ Factory/*.xml or generated)
    Utilities/  DSPUtils.{h,cpp}
    Tests/  DSPTests.cpp  KickEngineTests.cpp  OfflineRender.{h,cpp}
```
Note: this introduces a `Source/` subtree deeper than any existing plugin in the repo — intentional for scope.

## Testing

Automated coverage: parameter ranges · state serialization round-trip · preset loading · MIDI triggering · voice lifecycle · pitch envelope shape · amplitude envelope shape · oversampling latency/switching · rapid retrigger (no clicks) · NaN/Inf detection · DC offset bound · output stability · silence after voice completion · deterministic offline render · **sample load / resample accuracy / trim / reverse / missing-file handling · sample+synth layer sum · sample path adds no audio-thread allocation**.
Plus an **offline render utility** that renders a kick to WAV for inspection.

## Technical Notes / Risks

- Pitch envelope must operate in log/ratio domain (`f = fundamental * ratio(t)`), phase-continuous, to produce punch not "laser."
- Continuous distortion morph: order the 7 algorithms on a perceptual curve and crossfade adjacent pairs; keep gain-compensated so `character` sweeps don't jump in loudness.
- Sub/low mono enforcement: sum-to-mono below a crossover before width is applied to upper content.
- Safety limiter must be transient-preserving (fast lookahead-free or short-lookahead soft limiter), not a brickwall that eats the click.
- Analyzer FFT size ~1024–2048; decouple repaint rate (~30 fps) from audio.

## Next Steps

- [ ] `/plan KICKR` — Stage 0 research → `architecture.md` + `plan.md` (validate against this brief; confirm oversampling default target, distortion morph ordering, limiter design)
- [ ] `/implement KICKR` — staged build (shell → DSP → GUI), compiling + testing after each stage, per the development order below
- [ ] On ✅ Working: archive `MinimalKick`

### Development order (from spec)

1. Inspect repo & build system ✅ (done in this brief)
2. JUCE + CMake plugin shell
3. MIDI-triggered basic kick (body + amp env)
4. Pitch envelope
5. Amplitude envelope refinement
6. Click
7. Sub
8. Tail
8b. **Sample player + library** (load / resample / trim / reverse / AD env / HP-LP / crush; folder scan + drag-drop; sample↔synth enable; sum with synth layers)
9. Distortion (morph)
10. Tone / output / stereo
11. Oversampling
12. Parameters / APVTS state (incl. `currentSampleName`)
13. Presets
14. Waveform / spectrum displays + sample bank browser
15. Final UI
16. Tests + offline render utility
17. Profile & optimize (lock oversampling default)
18. Release build: VST3 / AU / Standalone

Compile, run relevant tests, and clear all errors + warnings after every major stage before continuing.
