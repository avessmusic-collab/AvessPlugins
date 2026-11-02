# TENSION Changelog

## 1.0.0 — 2026-11-02 (post-release tweak, stays 1.0.0 per factory convention)

User-directed: faint animated TV-static texture over the backdrop + scoped bug review.
- Visualizer: 4 pre-rendered 128 px speckle tiles (alpha baked in, peak 8.5 %)
  cycled at 12 Hz with random tile offsets; always moving, even at 0 % tension
  (idle gate now settles to 12 Hz window-only repaints instead of a full stop)
- Follow-up (same day): "no signal" horizontal distortion. Tiles regenerated as
  horizontally streaked snow (decaying-max dashes, peak 7.5 %); one slowly
  rolling band (22 px, 3x x-stretch, brighter) plus 0-2 transient tear bands
  per flip (random y/height, 2-4.5x x-stretch) smear the static left/right
  like a dead analog channel.
- **Bug fix (from review):** RiseModeSelector::mouseDown still hard-coded the old
  220 px width after the selector was widened to 290 px — clicks near segment
  edges selected the wrong mode (e.g. center of CINEMATIC chose AGGRESSIVE).
  Hit-testing now derives from getWidth(), same as paint().
- Consistency: EngineParams::mix / reset() mixSm seed aligned to the 50 % param
  default; editor's duplicate double-click return values removed (ctor is the
  single source); stale comments fixed.
Full suite re-run ALL PASS; reinstalled.

## 1.0.0 — 2026-10-30 (post-release tweak, stays 1.0.0 per factory convention)

User-directed UI/default changes after first install:
- MIX default 100% → 50% (double-click reset follows; parameter-spec.md amended)
- RISE MODE selector enlarged 220×30 → 290×34 pill with roomier segments so
  SLOW BURN / CINEMATIC / AGGRESSIVE fit comfortably (v4-ui.yaml amendment)
Full suite re-run ALL PASS; reinstalled.

## 1.0.0 — 2026-10-30

First complete build (version stays 1.0.0 per factory convention until public release).

### What it is
One-knob cinematic tension/riser effect. Automate the TENSION knob 0–100% over
4–32 bars: coordinated nonlinear curves drive procedural noise, dual LFOs
(amplitude/filter/stereo/delay), a rising HP + resonant BP sweep, ADAA
saturation, M/S width, and parallel ping-pong delay + reverb sends. Generates
risers from silence. Bit-exact passthrough at 0% (after tails decay).

### Parameters (5)
tension · mix · output · sync · riseMode (Slow Burn / Cinematic / Aggressive)

### Build history (all phases tested + pluginval strictness 10)
- **Stage 1** — CMake (VST3+AU+Standalone, native GUI, no WebView), APVTS shell. Also fixed the monorepo root build (CHOPLINE self-contained exclusion).
- **3.1** — Engine skeleton: 32-sample control grid, delta-form engagement routing (bit-exact t=0), custom OutputSafety (scrub / C1 knee at −1 dBFS / runaway guard), telemetry atomics, console test harness.
- **3.2** — LFOEngine (5 shapes, BPM-sync soft-snap ladder), FilterEngine (TPT SVF, Q≤4), SaturationEngine (first-order ADAA tanh, level-compensated, alias −54.8 dB @ max drive), StereoWidth (LR4 150 Hz, mono-safe).
- **3.3** — NoiseGenerator (deterministic xorshift32, pink/white/filtered), ReverbEngine (always-processed send/return + pre-delay, asym 150/700 ms returns), DelayEngine (Lagrange3rd ping-pong, feedback cap 0.85). **Fix:** delay time-slew velocity clamped ±0.9 samp/samp + 20 ms smoothing — read head reversed on tempo jumps (−34 dB zipper → −81 dB).
- **3.4** — Real TensionMapping: 34 destinations × 3 constexpr mode profiles, 600 ms mode crossfade, per-mode LFO shapes. Spec §4 onsets verified (noise ~25%, atmosphere ~60%, feedback ~75%).
- **3.5** — TENSION_Profiles metrics harness (4/8/16/32-bar ramps; LOUD/STEP/MONO/DEAD criteria). Noise exponents eased for mid-ramp participation. Aggressive mid-ramp dip (~2.3 dB) ruled intentional (tremolo character; tuning-log 3.5.4 has fallback knobs). Freeverb verdict GO. Optional RMS level compensator (default OFF).
- **3.6** — Robustness: rate matrix 44.1–192 kHz, 60 s soaks, bit-identical state restore, guard engage/release, denormal holds, fuzzing, re-prepare determinism. **Fixes:** noise √(fs/48k) spectral-density compensation (was −6 dB at 192 kHz); juce::dsp::Reverb placement-reconstruction in prepare (smoothers survive reset — DROP lesson). CPU 1.07% avg of one core @48 kHz.
- **4.1–4.3** — Native JUCE GUI per locked mockup v4: machined bezel, 9-layer 3D knobs, radial pulse field (72 spikes/rings/particles) live from engine telemetry at 60 Hz with VU ballistics; per-mode color themes (Slow Burn orange→red, Cinematic teal→blue default, Aggressive violet→magenta) with 450 ms crossfade following the riseMode parameter. pluginval with GUI tests SUCCESS.

### Final suite
331 automated checks, 0 failures. Metrics harness: ALL CRITERIA PASS.

### Known notes
- Kellett pink filter tilt varies slightly with host sample rate (level is rate-stable; color drifts mildly). Accepted for v1.
- Presets: none in v1 (spec's Presets/ treated as post-1.0 state documents).
- Manual DAW checklist pending: touch/latch automation recording, tempo change mid-build, the Aggressive mid-ramp dip by ear, 100% reverb tail character, 4/8/16/32-bar ramp musicality.
