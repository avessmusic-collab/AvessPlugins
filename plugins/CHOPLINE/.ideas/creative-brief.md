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

## UI Design — FINAL (2026-10-24)

**v17 mockup is the locked visual direction** (`.ideas/mockups/v17-ui.yaml` + `v17-ui-test.html`; v1–v16 deleted, recoverable from git history). Production UI is **native JUCE LookAndFeel** per spec §9 — the mockup is the visual/interaction reference.

Key design decisions baked into v17 (supersede spec §9 where they conflict):
- Wireframe-light skin (Sonisto-style): paper-white cards, hairline-grid light displays (no dark screens), flat white disc knobs with near-black ink notch (#0d0d0f), tiny wide-tracked uppercase labels (#2a2a2e), mono value pills, iOS-style mini toggles; blue #3a6df0 as thin accent (selection/playhead), red #ff5a4f strictly for Beat Repeat activity; chop identity = greyscale shades + index (no colour palette)
- MAIN page: lean header (wordmark · presets · MIDI dot · MAIN|FX · gear — **no pattern slots A–H in UI** (deferred to advanced; model keeps 8 slots), no play button (host transport only), no BPM in header); sample strip (clean, no mini-map); **single compact note-duration button** (1/4→1/8→1/16 cycle, hold for menu w/ triplet+dotted; no 1/32); big pattern boxes; single switchable lane strip (VEL·PITCH·PROB·RATCHET); bottom bar = BEAT REPEAT + MASTER (FILTER·DIST·WIDTH·OUT + BPM readout)
- FX page: two narrow centred columns — CHOP (playback/warp + compact envelope) and MASTER (filter, 6-type distortion, width|output, delay, reverb); titles + bypass mini-toggles inside each card
- **Master-level processing decisions:** filter is master-only (per-chop filter removed; HP removed; 12 dB/oct fixed), distortion is master-only (absorbed saturation — Tape type ≈ old sat; per-chop distortion removed), tilt/tone removed
- Chop editor overlay = Simpler-style sample editor: zoomed chop waveform, draggable START/END flags, fade-in/out grips (0–20 ms), loop brace + crossfade in Loop mode, click-to-audition; per-chop SliceMap state: startOffset, endOffset, fadeInMs, fadeOutMs, loopStart, loopEnd, loopXfade
- MIDI: chops trigger from MIDI notes (base C1 + N, velocity-sensitive, base configurable in settings); VST3/AU for Ableton + all major hosts
- Deferred (in YAML not_surfaced lists): pattern slots A–H, deep Beat Repeat params (offset, gate, pitch decay, accel, sweep, reverse repeats, mix mode, freeze, random grid)

## Next Steps

- [x] Concept confirmed (2026-10-24)
- [x] UI mockup finalized: v17 (2026-10-24)
- [ ] Phase 0 scaffold → then spec phases 1–6, stopping after each for user approval
- Note: JUCE 9 confirmed released (2026); pin into project via CPM/submodule (system JUCE 8.0.15 stays for other plugins)
