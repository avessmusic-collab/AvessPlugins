# KickDesigner2 Notes

## Status
- **Current Status:** 🚧 Stage 0 (Research & Planning complete)
- **Version:** N/A
- **Type:** Synth (Kick Instrument)

## Lifecycle Timeline

- **2026-08-28:** Ideated via `/dream`. Full creative brief captured from detailed user spec (algorithmic kick-design instrument for Techno / Hard Techno / Hardstyle / Hardcore / EDM / House / Trap / DnB / Industrial). Repo inspected: JUCE 8.0.15, CMake+Ninja, auto-discovery root CMake.
- **2026-08-28 (Stage 0):** Research & Planning complete — `architecture.md` + `plan.md` created. Complexity score **5.0** (capped) → phased implementation. ~14 DSP components specified; all 10 Open Questions resolved (oversampling default 2× provisional, zero-latency soft-clip limiter, locked distortion morph order, pre-distortion tone, 130 Hz mono crossover, single-param pitch contour, `fundamental` = global offset in MIDI mode, fixed 2 ms body attack, `mix` = processed↔silence). Flagged spec parameter-count discrepancy (header 42 Float vs body 41 Float → 45 APVTS params). PLUGIN codes: `Plgf` / `Kd02`.

## Known Issues

None (not yet implemented).

## Additional Notes

**Key design decisions (from ideation):**
- UI: **native JUCE** custom controls + LookAndFeel (departs from repo's WebView convention) — chosen for real-time waveform/spectrum analyzers.
- **Supersedes `MinimalKick`** (stalled 🚧 Stage 5). Migrate reusable code (sine body osc, exp pitch env, AD amp env, tanh drive). Archive MinimalKick once KickDesigner2 reaches ✅ Working.
- Distortion `Character` = single **continuous morph** knob through 7 nonlinearities (SoftClip→Tanh→Cubic→Asym→HardClip→Foldback→BitCrush), gain-compensated.
- Presets: factory (BinaryData) + **user disk presets**; header Save button functional.
- Monophonic v1, fast click-free retrigger. No wavetable/poly/arp/mod-matrix/fx-rack.

**Architecture:** KickEngine → KickVoice → {BodyOscillator+PitchEnvelope+AmplitudeEnvelope, SubOscillator, ClickGenerator, TailGenerator, NoiseGenerator} → TransientShaper → Distortion (inside dsp::Oversampling 1/2/4/8×) → Tone (3-band) → Stereo (mono lows) → OutputStage (gain + safety limiter + mix).

**Formats:** VST3, AU, Standalone.

**Contract files:**
- `.ideas/creative-brief.md` — complete (2026-08-28)
- `.ideas/architecture.md` — pending (`/plan`)
- `.ideas/plan.md` — pending (`/plan`)
- `.ideas/parameter-spec.md` — pending (planning / UI)

**~30 primary parameters** + tuning (MIDI/Fixed, tune, fine) + velocity mappings + 4 macros (Punch/Body/Crush/Tail) + Randomize/Mutate (constrained). See creative brief for the full table.

**Default sound target (tune by ear):** 55 Hz fundamental, 4× pitch start, ~50 ms pitch time, ~400 ms body decay, moderate click/sub/drive, limiter on, OS 2×/4×.
