# KickDesigner2 Notes

## Status
- **Current Status:** 🚧 Stage 0 (Research & Planning complete)
- **Version:** N/A
- **Type:** Synth (Kick Instrument)

## Lifecycle Timeline

- **2026-08-28:** Ideated via `/dream`. Full creative brief captured from detailed user spec (algorithmic kick-design instrument for Techno / Hard Techno / Hardstyle / Hardcore / EDM / House / Trap / DnB / Industrial). Repo inspected: JUCE 8.0.15, CMake+Ninja, auto-discovery root CMake.
- **2026-08-28 (Stage 0):** Research & Planning complete — `architecture.md` + `plan.md` created. Complexity score **5.0** (capped) → phased implementation. ~14 DSP components specified; all 10 Open Questions resolved (oversampling default 2× provisional, soft-clip `tanh` limiter, locked distortion morph order, pre-distortion tone, 130 Hz mono crossover, single-param pitch contour, `fundamental` = global offset in MIDI mode, fixed 2 ms body attack, `mix` = processed↔silence). Parameter count reconciled to 45 APVTS (41 Float / 3 Choice / 1 Bool). PLUGIN codes: `Plgf` / `Kd02`.
- **2026-08-28 (Stage 0 revision, post-review):** **AD-10** — oversampling scope widened from "master waveshaper only" to the **entire voice + master chain through the `tanh` safety limiter**. Every nonlinearity (`bodyHarmonics`, `tailDrive`, master morph, limiter) is oversampled by construction, not by a "fundamental is low" argument. OS region is the Phase-2.1 substrate (pinned 1×), real factors enabled Phase 2.10; CPU scales ~linearly with factor, `8×` labelled high-CPU, Stage-17 base-rate escape hatch documented. `architecture.md` + `plan.md` + `parameter-spec.md` updated consistently.

## Known Issues

None (not yet implemented).

## Additional Notes

**Key design decisions (from ideation):**
- UI: **native JUCE** custom controls + LookAndFeel (departs from repo's WebView convention) — chosen for real-time waveform/spectrum analyzers.
- **Supersedes `MinimalKick`** (stalled 🚧 Stage 5). Migrate reusable code (sine body osc, exp pitch env, AD amp env, tanh drive). Archive MinimalKick once KickDesigner2 reaches ✅ Working.
- Distortion `Character` = single **continuous morph** knob through 7 nonlinearities (SoftClip→Tanh→Cubic→Asym→HardClip→Foldback→BitCrush), gain-compensated.
- Presets: factory (BinaryData) + **user disk presets**; header Save button functional.
- Monophonic v1, fast click-free retrigger. No wavetable/poly/arp/mod-matrix/fx-rack.

**Architecture (AD-10):** `[dsp::Oversampling ▲ 1/2/4/8×]` → KickEngine → KickVoice → {BodyOscillator+PitchEnvelope+AmplitudeEnvelope+bodyHarmonics, SubOscillator, ClickGenerator, TailGenerator+tailDrive, NoiseGenerator} → Σ → Tone (3-band, pre) → TransientShaper → Distortion morph → Stereo (mono <130 Hz) → Mix → Output gain → Safety limiter (`tanh`) → `[dsp::Oversampling ▼]` → DC blocker → analyzer taps. **Whole chain oversampled**; only DC blocker + analyzer at base rate.

**Formats:** VST3, AU, Standalone.

**Contract files:**
- `.ideas/creative-brief.md` — ✓ complete (2026-08-28)
- `.ideas/parameter-spec.md` — 🔒 locked v1 + Stage 0 Addendum + AD-10 note (45 APVTS params)
- `.ideas/research-notes.md` — advisory (deep-research Level 2)
- `.ideas/architecture.md` — ✓ Stage 0 (revised 2026-08-28, AD-10)
- `.ideas/plan.md` — ✓ Stage 0 (revised 2026-08-28, AD-10)

**45 APVTS parameters** (41 Float / 3 Choice / 1 Bool): Pitch(4) · Body(3) · Sub(3) · Click(4) · Tail(4) · Noise(4) · Transient(2) · Distortion(3) · Tone(3) · Stereo(3) · Output(3) · Global(1) · Tuning(3) · Velocity(1) · Macros(4). Plus Randomize/Mutate/Save UI actions. Full table in `parameter-spec.md`.

**Default sound target (tune by ear):** 55 Hz fundamental, 4× pitch start, ~50 ms pitch time, ~400 ms body decay, moderate click/sub/drive, limiter on, OS 2×/4×.
