# PRISM - Creative Brief

## Overview

**Type:** Audio Effect (Multi-Source Analyzer / Visualizer, bit-transparent passthrough)
**Core Concept:** A premium real-time audio visualization instrument — a multi-source spectrum analyzer, standards-compliant loudness/metering suite, and modular visual dashboard that is technically accurate, beautiful, hypnotic, and genuinely fun to leave open while making music.
**Status:** 💡 Ideated
**Created:** 2026-10-14

## Vision

PRISM is inspired by MiniMeters, SPAN, and high-end music visualization software, but with an entirely original visual identity: scientific instrumentation meets modern synthesizers, ambient generative art, and refined motion graphics. Near-black charcoal background, delicate grid lines, soft luminous accents, exceptionally clear typography. Spectral curves that look fluid, precise, and alive — subtle glow, transparent layering, soft trails, elegant peak markers. Energy visibly flows through the interface; kicks, basslines, transients, pads, and complex harmonics all produce immediate, satisfying visual response. Decorative animation never replaces or distorts measurement data. No excessive bloom, no cheap neon, no clutter.

It is simultaneously a **trustworthy engineering tool** (calibrated axes, standards-compliant loudness, honest measurements — never fabricated values) and a **creative instrument** (hypnotic, performable, pleasant to watch).

## v1.0 Scope (decided during ideation)

- **Sources:** 2 simultaneous stereo sources — main input + one sidechain input bus. This is the maximum Ableton Live (primary target DAW) can route into a single plugin. The internal source architecture is designed for 8–16 sources so hosts with real multibus routing (Reaper, Bitwig, Cubase) can be unlocked with minimal changes.
- **PRISM Sender companion + shared-memory IPC:** deferred to a later version (v2). It is the designed workaround for Live's routing limits; the v1 architecture (source registry, per-source analysis state, color/name/visibility model) must not preclude it.
- **All visual modules ship in v1.0:** spectrum analyzer (centerpiece), scrolling spectrogram, vectorscope/goniometer, LUFS + true-peak meters, stereo correlation/width meter, oscilloscope, peak/RMS meters, waveform history.
- **UI technology:** WebView UI (factory pipeline standard — canvas/WebGL rendering inside the plugin window, no external browser). This supersedes the original spec's native-JUCE-LookAndFeel requirement; visual goals are fully achievable on canvas at 60fps.
- **Formats:** VST3 + AU (macOS), optional Standalone target.
- Works perfectly as a conventional single-track analyzer with zero configuration.

## Spectrum Engine

- `juce::dsp::FFT`, configurable sizes 1024/2048/4096/8192/16384 (default 4096), 75% overlap, Hann or Blackman-Harris windows, correct coherent-gain normalization, sample-rate-aware frequency mapping.
- Logarithmic frequency axis 20 Hz–20 kHz (or up to Nyquist), calibrated dBFS amplitude axis.
- Per-source spectral curves: translucent gradient fills, independent colors, optional peak-hold traces, adjustable attack/release smoothing, frequency averaging, configurable visual tilt (e.g. 0–4.5 dB/oct), zoom.
- Overlay, stacked, and individual mini-spectrum views; isolate one curve or compare all.
- Frequency grid markers, hover-to-inspect (frequency/amplitude), musical note identification, dominant frequency detection, optional piano-key frequency markers.
- Smooth interpolation between bins and frames **without falsifying underlying measurements**.
- Optional L/R and Mid/Side analysis modes.

## Metering (standards-compliant)

- Sample peak, RMS, peak hold with decay, clipping indicators, crest factor.
- **Loudness per ITU-R BS.1770-5 / EBU R128:** correct K-weighting, channel weighting, absolute/relative gating, 400 ms momentary LUFS, 3 s short-term LUFS, integrated LUFS (resettable), loudness range (LRA).
- **True peak** via standards-compliant oversampling with appropriate filtering — never conflated with sample peak.
- Stereo correlation: normalized covariance, −1 to +1, with explicit silence handling.
- Stereo width from properly normalized mid/side energy with a documented convention; stereo balance.
- Per-source numeric readouts with correct units and configurable precision; independent measurement histories.
- Accuracy validated against known reference signals and a trusted reference implementation. **Never display fabricated values.**

## Visual Modules (modular dashboard)

| Module | Key behaviors |
|--------|---------------|
| Spectrum analyzer | Visual centerpiece; smooth anti-aliased per-source curves with translucent gradient fills, peak-hold traces, hover inspector (freq / note / dB) |
| Spectrogram | Vertical waterfall (MiniMeters orientation): frequency left→right on log axis aligned with the spectrum, time scrolling downward; per-pixel rendering, perceptual brightness, theme-derived low→mid→high hue ramp, Ch1/Ch2 selector, adjustable speed |
| Beat waveform strip | Rekordbox-style 3-band mirrored waveform timeline (~12 s, playhead right, beat ticks); bass/mids/highs in theme-derived band colors, Ch1/Ch2 selector |
| Vectorscope / goniometer | Dominant lower pane, near-square Lissajous cloud with persistence, graduated rings, M/S/L/R markers |
| LUFS readouts | M / S / I / LRA text column, reset + reset-integrated actions |
| True peak / correlation | Per-channel TP/SP (plain "db", 2 decimals), correlation −1..+1, width, balance |
| Oscilloscope | Triggered waveform, fading trail |

**Readouts:** Loudness, True Peak, Stereo as stacked bordered cells in the right sidebar — no engine metrics block (FFT/window live in settings; SMP+FFT in the footer rail). Global rule: every numeric readout capped at 2 decimals.
**Layout:** v8 instrument grid — modules resized by dragging the shared pane borders (row height splits + lower-row column splits, double-click to reset, minimum sizes enforced); fullscreen presentation mode (⤢ button / F key) hides sidebar + footer so the visualizer grid fills the editor window. Splits + fullscreen are serialized UI state.
**Sources:** named Ch1 / Ch2 (renameable); clickable legend + footer chips toggle visibility, [S] tags solo-for-display.

## Parameters (locked by finalized mockup v7 — see .ideas/mockups/v7-ui.yaml)

17 APVTS parameters: fft_size (choice, default 4096), window (choice, Hann default), smoothing (0–100%, single knob replacing attack/release), spectrum_tilt (0–4.5 dB/oct, default 4.5 = Pro-Q 3 analyzer slope; Settings slider, no knob), display_range (default −90..0 dBFS), spectrogram_speed, peak_decay, freeze (bool), analysis_mode (L/R / M/S / combined), preset (Studio/Minimal/Neon/Ambient/Scientific/Performance), **color_theme** (7 choices: Coral/Mint default, Cyan/Violet, Amber/Cyan, Magenta/Blue, Green/Ice, Gold/Violet, Ice/Steel — each a full token set incl. band-color trio for spectrogram/waveform), reduced_motion, glow_intensity, src1_visible, src1_solo, src2_visible, src2_solo.

2 ValueTree state entries: src1_name, src2_name (defaults "Ch1"/"Ch2"). Per-channel colors derive solely from color_theme (no individual color overrides).

Bridge events (not parameters): reset_all, reset_integrated, prismUiState {row_splits, lower_col_splits, fullscreen}.

All settings persisted via AudioProcessorValueTreeState + serialized UI state (layout splits, fullscreen, window size). Project reload restores source configuration safely; never confuses previously connected instances with new ones.

## UI Concept (finalized — mockup v8)

**Form:** A scientific-instrument dashboard (from the user's second reference image): edge-to-edge black screen (1100×720, no device bezel), modules tiled flush in thin 1px-bordered panes with corner ticks, fine engineering-grid lines inside plots, and small live monospace annotations in pane corners (FFT/WIN, SMTH/TILT/RNG, SPD, CORR, TRIG…). Monospace typography throughout; muted labels, luminous theme-colored values. No CRT pastiche — clean vector rendering.
**Main grid (top→bottom, left of sidebar):** smooth-curve spectrum pane (hero; legend + hover inspector in its header) → 3-band beat waveform strip pane → spectrogram waterfall | large vectorscope | oscilloscope panes. Pane borders drag-resize (double-click resets); fullscreen mode (⤢ / F) hides sidebar + footer leaving only the visualizer grid.
**Right sidebar:** PRISM nameplate → Loudness / True Peak / Stereo readout cells (2-decimal cap, per-channel tinting) → vector needle meter (momentary LUFS, theme-tinted) → four compact knobs (smoothing, tilt, glow, spectrogram speed) → bordered buttons FREEZE / RESET / RESET I / THEME (click-cycles the 7 themes) / SOURCES / SETTINGS. Settings popover holds FFT, window, analysis mode, display range, peak decay, reduced motion, presets, and the full theme chip row; Sources popover: rename / visible / solo per channel.
**Color:** 7 switchable themes (color_theme parameter, Coral/Mint default) — each defines Ch1/Ch2 hues, neutral tint, UI accent, and the low/mid/high band trio used by spectrogram + waveform. Per-channel values tinted in channel color; shared values neutral.
**Honesty cues:** calibrated axes, true untilted dBFS in the inspector, data-driven LIVE / NO SIGNAL / NO DATA status flag (pulsing "DEMO SIGNAL" in the mockup's demo mode).
**Quality targets:** 60 fps, HiDPI/Retina-sharp canvases, reduced-motion mode, restrained glow (no bloom), adaptive quality for weaker machines.

## Use Cases

- Mix monitoring: loudness, true peak, correlation, and spectrum at a glance on the master bus.
- Sound design / synth playing: hypnotic, immediate visual feedback while performing.
- Comparing two sources (v1: main vs sidechain — e.g. your mix vs a reference track, or kick vs bass).
- Studio eye-candy / performance mode: fullscreen presentation while producing.

## Inspirations

- MiniMeters (multi-module dashboard, fun factor)
- Voxengo SPAN (trusted spectrum analysis, per-source overlays)
- High-end music visualization software; scientific instrumentation; ambient generative art; refined motion graphics

## Technical Notes

**Real-time architecture (non-negotiable):**
- No heap allocations, locks, file I/O, GUI work, or expensive FFT inside `processBlock()`; everything preallocated in `prepareToPlay()`.
- Bounded lock-free SPSC queues between audio → analysis → UI threads; FFT/loudness aggregation on a dedicated worker thread with preallocated memory and bounded workloads.
- Immutable analysis snapshots published to the renderer with a race-free ownership strategy; all visual interpolation outside the audio thread.
- Bit-transparent audio passthrough, zero intentional latency.
- Handles arbitrary block sizes, variable sample rates (44.1/48/96/192 kHz), mono/stereo layouts, silence, transport stop, suspension, editor closure, device changes. Minimal CPU with editor closed while preserving measurement state. RAII, clean thread shutdown, denormal handling.
- Multibus: `BusesProperties` + `isBusesLayoutSupported()` + `getBusBuffer()`; v1 exposes main + 1 sidechain (Live's ceiling), architecture scales to 8–16.

**Project structure:** clean modular JUCE 8 / CMake / C++20 repo; directories for Source, DSP, Analysis, Metering, GUI, Components, Themes, Tests, Assets (IPC dir reserved for v2 Sender). Internal demo signal generator for UI development, clearly separated from real measurements.

**Validation:** automated tests for FFT frequency/amplitude accuracy (sines, white noise, impulses, multitone), LUFS/true-peak against reference fixtures, correlation with identical/uncorrelated/polarity-inverted signals, serialization, passthrough bit-transparency; CPU/memory profiling at 1/2 (and future 8/16) sources; pluginval; honest documentation of limitations.
*(Factory note: console test harness must call `setPlayConfigDetails` before `prepareToPlay` — see memory.)*

## Roadmap Beyond v1.0

- **v2:** PRISM Sender companion VST3 + local shared-memory IPC — versioned messages, instance discovery, source registration, automatic cleanup, timestamps + bounded queues for alignment, graceful disconnection handling, no raw-audio transfer where compact analysis frames suffice. Unlocks true 8–16 source operation in Ableton Live.
- Expand input buses to 8–16 for hosts with real multibus routing.

## Next Steps

- [ ] Create UI mockup (`/dream PRISM` → option 3)
- [ ] Start implementation (`/implement PRISM`)
