# CHOPLINE - Creative Brief

## Overview

**Type:** Synth (Vocal Chop/Warp/Glitch Instrument)
**Core Concept:** Drop in a vocal, slice it into chops, drag the chops onto a host-synced beat grid, and warp/pitch/distort/stutter each one — with a performable beat-repeat stage on the output.
**Status:** 💡 Ideated
**Created:** 2026-10-24

> **The full build spec is the contract:** [chopline-spec.md](chopline-spec.md) (user-authored, to be followed **as written**, phase by phase with user approval between phases). This brief only records identity and session decisions; the spec wins on any conflict.

## Lineage

CHOPLINE supersedes MOUTHPIECE (same project slot, new concept; the MOUTHPIECE folder was deleted 2026-10-24 at the user's request — its mockups v1–v4 remain in git history, and its technical spec remains in the user's Claude Doc https://claude.ai/artifact/1JxHCFkp8MYAuVD4aUTEN8). Key conceptual shift: MOUTHPIECE resolved rhythm and pitch independently (sequencer + held-key transpose); CHOPLINE is a direct chop arranger — drag chops onto grid cells — with deep per-chop processing and glitch performance on top.

## The Two Strips

1. **Strip A — Sampler:** waveform of the loaded sample with slice markers (Transient / Grid / Equal / Manual slicing, up to 64 chops), auditionable and draggable.
2. **Strip B — Grid:** 1–4 bar step lane, 1/4–1/32 with triplet/dotted, swing, 8 pattern slots; chops dragged from Strip A onto cells, spanned across steps, with per-step override lanes (velocity, pitch, reverse, ratchet, probability, stretch, drive, filter, pan).

Then: per-chop warp engine (varispeed / Signalsmith stretch with formant preserve / granular / freeze), detune + unison + scale lock, 6-type oversampled distortion, SVF filter, AHDSR, choke groups → Beat Repeat (Ableton-style, MIDI-performable) → Master FX (tilt EQ, tape saturation, width, delay/plate send, limiter).

## Decisions Made During Ideation (2026-10-24)

1. **Full spec as written** — the user explicitly chose the complete CHOPLINE spec over a cut-down version. Build phases 0–6 sequentially, stopping for user approval after each phase (spec §11).
2. **UI:** native JUCE `LookAndFeel`, **not WebView** (spec §9) — note this diverges from the house WebView workflow; ui-mockup HTML mockups can still drive visual direction, but implementation is native. Spec says: ask the user for visual direction before polishing the look in Phase 5.
3. **Open question — JUCE 9:** the spec mandates JUCE 9; the system config and all prior plugins use JUCE 8. Verify JUCE 9 availability/stability at Phase 0; if not viable, raise with the user before substituting JUCE 8.
4. Factory presets: 30, user supplies samples and tunes them (spec §10).
5. Reference points (feel, not copy): Ableton Simpler slice mode + Beat Repeat, Replicant 3, Cymatics Relapse, ChopBeast, Slicex.

## Technical Notes

See spec §§2–8: MIT/BSD-only deps (Signalsmith Stretch; **no GPL**), strict real-time rules with debug allocation detector, sample-accurate PPQ-derived scheduling, immutable snapshot UI↔engine handoff, 16-voice pool, CPU < 5% of one M1 core @ 48 kHz/256 with the lot on, Catch2 + pluginval 10 + CI matrix. Per the spec: save it as SPEC.md in the plugin repo root when scaffolding begins, and keep CLAUDE.md with decisions/open questions.

## Next Steps

- [ ] Describe/confirm concept with user (done 2026-10-24)
- [ ] Optional: UI mockup for visual direction (`/dream CHOPLINE`)
- [ ] Start Phase 0 scaffold (`/implement CHOPLINE` — then follow spec phases, stopping after each)
