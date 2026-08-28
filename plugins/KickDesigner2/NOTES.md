# KickDesigner2 Notes

## Status
- **Current Status:** 🚧 Stage 1 complete + v2 SAMPLE amendment — ready for Stage 2
- **Version:** N/A
- **Type:** Synth (Kick Instrument) — algorithmic synthesis + sample-playback layer
- **Spec:** parameter-spec v2 · 59 APVTS params

## Lifecycle Timeline

- **2026-08-28:** Ideated via `/dream`. Full creative brief captured from detailed user spec (algorithmic kick-design instrument for Techno / Hard Techno / Hardstyle / Hardcore / EDM / House / Trap / DnB / Industrial). Repo inspected: JUCE 8.0.15, CMake+Ninja, auto-discovery root CMake.
- **2026-08-28 (Stage 0):** Research & Planning complete — `architecture.md` + `plan.md` created. Complexity score **5.0** (capped) → phased implementation. ~14 DSP components specified; all 10 Open Questions resolved (oversampling default 2× provisional, soft-clip `tanh` limiter, locked distortion morph order, pre-distortion tone, 130 Hz mono crossover, single-param pitch contour, `fundamental` = global offset in MIDI mode, fixed 2 ms body attack, `mix` = processed↔silence). Parameter count reconciled to 45 APVTS (41 Float / 3 Choice / 1 Bool). PLUGIN codes: `Plgf` / `Kd02`.
- **2026-08-28 (Stage 0 revision, post-review):** **AD-10** — oversampling scope widened from "master waveshaper only" to the **entire voice + master chain through the `tanh` safety limiter**. Every nonlinearity (`bodyHarmonics`, `tailDrive`, master morph, limiter) is oversampled by construction, not by a "fundamental is low" argument. OS region is the Phase-2.1 substrate (pinned 1×), real factors enabled Phase 2.10; CPU scales ~linearly with factor, `8×` labelled high-CPU, Stage-17 base-rate escape hatch documented. `architecture.md` + `plan.md` + `parameter-spec.md` updated consistently.
- **2026-08-28 (Stage 1 — Foundation + Shell):** Buildable synth shell created. `CMakeLists.txt` (`IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`, VST3/AU/Standalone, `juce_dsp`/`juce_audio_formats` linked, `juce_generate_juce_header` after `target_link_libraries`, no WebView). Full **45-parameter APVTS** table-driven. Output-only stereo bus in the constructor; `UndoManager` attached; state round-trip `stateVersion = 1`. Deeper `Source/` subtree scaffolded. Native editor stub. Build: clean (0 warnings) VST3+AU+Standalone; pluginval strictness 10 SUCCESS; auval SUCCEEDED. (UTF-8 mojibake in the editor stub fixed in follow-up.)
- **2026-08-28 (UI concept):** v2 mockup at `.ideas/mockups/v1-ui-concept.html` (Artifact) — soft slate-grey rack instrument (chassis `#31353F` sampled from the user's BASSMAKER reference), kick-waveform oscilloscope, PUNCH/BODY/CRUSH/TAIL macro modules, 8-cell engine strip. Native JUCE, fixed 1600×904 scaling to fit.
- **2026-08-28 (v2 — SAMPLE engine, user request):** Added a sample-playback layer. Reverses the v1 "no samples" non-goal. `SamplePlayer` = blendable 6th layer (in-region resample → trim/reverse → AD → HP/LP → crush); `SampleLibrary` = managed drag-drop bank at `…/KickDesigner2/Samples/`. `synthEnable` on / `sampleEnable` off by default ⇒ v1 sound unchanged. **14 new params → 59 APVTS** (51 Float / 3 Choice / 5 Bool); new state property `currentSampleName`; `stateVersion` → 2. AD-11 (layer + bank) / AD-12 (no factory samples). All 4 contracts revised; new Stage-2 phase **2.7b**. **Stage 1 v2-amended:** IDs + layout + state updated, rebuilt clean, pluginval strictness 10 SUCCESS, auval "59 Global Scope Parameters" SUCCEEDED, zero-drift verified.

## Known Issues

None (not yet implemented).

## Additional Notes

**Key design decisions (from ideation):**
- UI: **native JUCE** custom controls + LookAndFeel (departs from repo's WebView convention) — chosen for real-time waveform/spectrum analyzers.
- **Supersedes `MinimalKick`** (stalled 🚧 Stage 5). Migrate reusable code (sine body osc, exp pitch env, AD amp env, tanh drive). Archive MinimalKick once KickDesigner2 reaches ✅ Working.
- Distortion `Character` = single **continuous morph** knob through 7 nonlinearities (SoftClip→Tanh→Cubic→Asym→HardClip→Foldback→BitCrush), gain-compensated.
- Presets: factory (BinaryData) + **user disk presets**; header Save button functional.
- Monophonic v1, fast click-free retrigger. No wavetable/poly/arp/mod-matrix/fx-rack.

- **v2 sample engine (AD-11/AD-12):** blendable 6th layer `SamplePlayer` + managed drag-drop bank `SampleLibrary`. `synthEnable` on / `sampleEnable` off default ⇒ v1 sound unchanged. No factory samples — user fills the bank.

**Architecture (AD-10 + AD-11):** `[dsp::Oversampling ▲ 1/2/4/8×]` → KickVoice → {**SamplePlayer** (resample→trim/reverse→AD→HP/LP→crush), BodyOsc+PitchEnv+AmpEnv+bodyHarmonics, SubOsc, ClickGen, TailGen+tailDrive, NoiseGen} → Σ (sample + synth, gated by `sampleEnable`/`synthEnable`) → Tone (3-band, pre) → TransientShaper → Distortion morph → Stereo (mono <130 Hz) → Mix → Output gain → Safety limiter (`tanh`) → `[dsp::Oversampling ▼]` → DC blocker → analyzer taps. **Whole chain oversampled**; only DC blocker + analyzer at base rate.

**Formats:** VST3, AU, Standalone.

**Contract files (revised 2026-08-28 for v2):**
- `.ideas/creative-brief.md` — ✓ (+ Sample Engine)
- `.ideas/parameter-spec.md` — 🔒 locked **v2** (+ Stage 0 Addendum + AD-10 note + v2 Addendum) — 59 APVTS
- `.ideas/research-notes.md` — advisory (deep-research Level 2)
- `.ideas/architecture.md` — ✓ (AD-10 + AD-11/AD-12)
- `.ideas/plan.md` — ✓ (Phase 2.7b + Stage 1 v2 Amendment)
- `.ideas/mockups/v1-ui-concept.html` — UI concept (Artifact 8c27d484)

**59 APVTS parameters** (51 Float / 3 Choice / 5 Bool): Pitch(4) · Body(3) · Sub(3) · Click(4) · Tail(4) · Noise(4) · **Sample(14)** · Transient(2) · Distortion(3) · Tone(3) · Stereo(3) · Output(3) · Global(1) · Tuning(3) · Velocity(1) · Macros(4). Plus Randomize/Mutate/Save + sample browse/drag-drop UI actions; sample selection = state prop `currentSampleName`. Full table in `parameter-spec.md`.

**Default sound target (tune by ear):** 55 Hz fundamental, 4× pitch start, ~50 ms pitch time, ~400 ms body decay, moderate click/sub/drive, limiter on, OS 2×/4×.
