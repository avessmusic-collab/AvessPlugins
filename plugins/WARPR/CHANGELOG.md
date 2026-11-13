# VOID (internal codename WARPR) - Changelog

Product name VOID, source folder `plugins/WARPR`, VST3 / AU / Standalone, instrument (MIDI in, stereo out).
Identity is final: `PLUGIN_CODE Void`, `PLUGIN_MANUFACTURER_CODE Vstf` (changing them orphans saved DAW projects).

## 1.0.0

First release. The version stays 1.0.0 until the public release; every improvement before that lands in this entry.

### What it is

A sample-based wave-warp synthesizer. A sample (or the built-in single-cycle wave) is played at pitch, bent / folded /
distorted / phase-modulated by one of 60 warp modes, shaped by an A/B transfer-curve shaper, filtered by one of 34
filters, and shaped by an amp envelope; everything is modulated by LFOs through a drag-and-drop matrix.

### Sound engine

- **Sample engine:** WAV / AIFF (8 / 16 / 24 / 32-bit int and float, mono or stereo, 22.05 - 192 kHz), loaded off the audio
  thread, immutable and ref-counted, octave mip pyramid for alias-free transposition (alias <= -80 dBc at +3 octaves),
  8-tap Kaiser sinc interpolation. Start / end region, root note, transpose, fine tune, forward / reverse.
- **Loop modes:** Forward Loop (seam-aware crossfade), Ping-Pong, One-Shot. **One-Shot is a true trigger mode:** one tap plays
  the whole region, key-up and sustain pedal are ignored, and the voice releases itself at the region end (Classic playback;
  Granular One-Shot stays gated).
- **Voices:** 32 voices + 2 ghost slots, Poly / Mono / Legato, glide, pitch bend (2 / 12 / 24 st), sustain pedal, all-notes-off,
  steal order = same note, quietest release, oldest; 3 ms ghost fades. Unison 1 - 8 (detune, spread, 1/sqrt(N) gain).
- **Oversampling:** 1x / 2x / 4x / 8x of the oscillator pipeline, latched per note, half-band IIR decimator per voice. Warp
  modes and the shaper enforce a minimum factor where the measured alias level requires it.
- **Warp (60 modes):** wave bending 0 - 14, wavefolding 15 - 24, distortion 25 - 43 (first-order ADAA), phase & time 44 - 53,
  modulation 54 - 59 (FM / PM / AM / RM / Self FM / Cross Mod, BLEP on hard sync and modulator shapes). Amount, A, B, mix,
  enable ramped; mode switches crossfade.
- **Shaper:** A/B morphable transfer curves (monotone cubic or linear points, symmetric / asymmetric), 9 presets, LUT +
  antiderivative ADAA, editable in the UI.
- **Filter (34 types):** LP / HP 6 - 48 dB, BP, notch, peak, shelves, combs, all-pass / serial all-pass, formant / vowel /
  morph formant, phaser, resonant ladders, distortion LP, scream, sample & hold, ring mod. Drive, morph, key tracking, mix,
  type-switch crossfade, analytic response display from the same coefficients as the audio.
- **Granular:** Classic / Loop / Cloud (up to 20 grains per channel), position, scan, scan distance / loop / key, size,
  shape, variation, spread, reverse, density; per-grain phase warps; deterministic PRNG.
- **Modulation:** LFOs with 13 shape presets, bends and custom curves, FREE / RETRIG / ENV / MONO modes, Hz or host-tempo sync
  (17 divisions, locked to the host position), direction, phase, smooth; modulation matrix to every Float / Int parameter
  (structural destinations are latched at note-on). The shipped UI shows up to 4 LFO pages and a new LFO defaults to SINE;
  the engine and the saved-state format still carry 10 LFOs.
- **Master:** DC blocker, master gain, safety knee, **brickwall limiter at -0.1 dBFS ceiling** (instant attack, ~100 ms release,
  bit-exact pass-through below the ceiling; never `juce::dsp::Limiter`), NaN / inf guard.

### UI

- WebView UI (WKWebView / WebView2): waveform with real playheads and grains, **processed-wave preview** (the waveform shows
  the sample through the current warp + shaper and updates live while knobs drag), warp / shaper / filter editors, LFO
  editor with drag-and-drop modulation rims, organism / meters fed by the real output, preset bar, resizable.
- **Presets and state:** `.warpr` container (all 135 parameters + UI state + the embedded sample as 24-bit FLAC or float32,
  default wave regenerated), user preset folder with save / load / step, **factory preset "Init"** (a guaranteed clean reset).
  Corrupt or truncated state falls back to defaults (fuzzed).
- **Settings dialog (per user, not part of projects):**
  - *Oscillator budget* 64 - 1024 units or off (default 256): a new note first reduces its own unison, then steals; sounding
    voices are never altered.
  - *Interpolation* High (8-tap sinc, default, bit-exact with earlier builds) or Eco (4-point Hermite; about a third of the
    read arithmetic, slightly duller on bright material at non-integer pitch ratios, identical at integer positions).
    Applies to every read: Classic, warped reads, the sample modulator and grains.
  - *CPU guard* (off by default): while the 1 s average of block-time / block-duration stays above 85 %, new notes latch a
    lower oversampling factor (cap 4x, then 2x, then 1x, one level per 0.25 s; one level back per 2 s below 60 %). Sounding
    voices are never touched, nothing is dropped or stolen because of the guard, and the guard deliberately overrides the
    warp / shaper minimum-oversampling floors (an aliasing note beats a dropout).

### Measured performance

Run on the release build (Apple Silicon, 48 kHz / 256-sample blocks, % of ONE core, median of 5 runs):

    WARPR_Tests --bench                       # the table below
    WARPR_Tests --bench --bench-full          # + the 3 x 4 x 4 grid (voices x unison x OS)
    WARPR_Tests --bench --bench-filter=typical

`BENCH` lines are machine readable (`name=... cpu_pct=... min_pct=... max_pct=... sounding=... eff_os=...`).
Plan gate: the plan estimated < 15 % of one core for the typical patch; the release gate is < 20 %, against the
MEASURED 16.9 % below (Apple Silicon, release build, quiet machine, median of repeated runs, spread < 0.4 %).
The optimization backlog for a post-1.0 pass: sinc read kernel (~23 % of audio time), warp kernel dispatch
(~25 %), pow-based bend map (~9 %).

Measured 2026-11-13, 48 kHz / 256, `WARPR_Tests --bench`:

| patch | cpu % of one core | sounding | eff OS | eff unison |
|---|---|---|---|---|
| baseline_plain | 1.44 | 8 | 1 | 1 |
| typical | 17.01 | 8 | 2 | 2 |
| typical_eco | 15.44 | 8 | 2 | 2 |
| typical_loaded_high | 18.43 | 8 | 2 | 2 |
| typical_loaded_eco | 16.82 | 8 | 2 | 2 |
| typical_v16 | 34.08 | 16 | 2 | 2 |
| typical_v32 | 68.75 | 32 | 2 | 2 |
| typical_u1 | 9.66 | 8 | 2 | 1 |
| typical_u4 | 31.77 | 8 | 2 | 4 |
| typical_u8 | 61.36 | 8 | 2 | 8 |
| typical_os1 | 9.49 | 8 | 1 | 2 |
| typical_os4 | 32.22 | 8 | 4 | 2 |
| typical_os8 | 62.47 | 8 | 8 | 2 |
| grain_classic | 5.43 | 8 | 2 | 1 |
| grain_loop | 4.22 | 8 | 2 | 1 |
| grain_cloud8 | 18.84 | 8 | 2 | 1 |
| grain_cloud20 | 45.32 | 8 | 2 | 1 |
| heavy_stack_v16_u8_os8_ungoverned | 473.89 | 16 | 8 | 8 |
| heavy_cloud20_u4_os8_ungoverned | 668.14 | 8 | 8 | 4 |
| heavy_stack_v16_u8_os8_governed256 | 118.55 | 4 | 8 | 8 |
| heavy_cloud20_u4_os8_governed256 | 105.49 | 2 | 8 | 4 |
| heavy_lfo10x3_v16_u2_os8 | 128.35 | 16 | 8 | 2 |

### Verification summary

Console suite `WARPR_Tests` (quick and `--full`): parameter table, state round trips and fuzz, every warp mode and filter
type (stability, bounds, identity, alias gates), granular, LFO / matrix, presets, preview, and the D10 sections
(interpolation, CPU guard, governor determinism, memory audit, bench harness). TSAN and ASAN / UBSAN runs and pluginval
strictness 5 were green at D9; re-run them on the release candidate (see the checklist).

### Known limitations and deferrals

Not blocking 1.0.0, recorded honestly:

- **Windows:** the Windows build / CI / WebView2 pass has not been run on this machine; the CI notes from earlier plugins
  (parent directory for the WebView2 package, static linking, 64 MB test stack, pluginval without wait) are applied in
  the CMake but unverified for VOID. macOS (VST3, AU, Standalone) is the verified platform.
- **Not done in D10 (needs a profiler / other hardware):** Instruments / VTune profiling beyond the `--bench` numbers,
  SIMD / auto-vectorisation inspection, the 2019 x64 CPU measurement.
- **Not shipped:** installer packaging (the `/package` flow does it on request), a factory sound bank (Init only).
- **Filters:** no dry-delay compensation for the half-band group delay at partial filter mix (a small comb at mix < 1 for
  the locally oversampled types); Dist LP and Scream have no loudness makeup; the Ring Mod display has no carrier marker
  (it draws flat); the comb display (256 points) cannot resolve resonance ~1 teeth; the all-pass family draws flat
  (mix is not part of the response curve).
- **Warp:** Mirror is not step-free (its seam bridge splits the step over a few samples); Hard Sync BLEP is applied at the
  master wrap only; changing the modulator source switches abruptly (no 5 ms crossfade); a second mode switch inside a
  running 10 ms crossfade drops the older outgoing kernel (a step is possible); mip level choice for warped reads is
  quantised to half octaves, rounded up.
- **Granular:** the visualisation reports oscillator 0's grains only; coherent pitched material can beat when the hop is
  not a whole number of cycles (inherent to overlap-add).
- **Modulation:** the mod wheel is stored but has no destination (the contract has none); LFO golden vectors were verified
  against a line-by-line transliteration of the UI code and 34 hand-derived anchors because the node exporter could not run
  on the build machine.
- **Loudness:** no automatic level matching between warp modes or patches; the -0.1 dBFS limiter catches peaks, it does not
  normalise.
- **Per-channel sample reads** (deferred from D7a to D10 as an optimisation idea) were not pursued; the `--bench` table is the
  evidence for whether it is needed.

### Install notes

Always install through the repository's build-and-install script, then restart the DAW (macOS: bundles are re-signed after
install). When the product name is changed, uninstall the old bundles first.
