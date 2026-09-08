# CORRUPTR Notes

## Status
- **Current Status:** 🚧 Stage 2 (DSP Implementation Complete — all 9 phases, all 14 architecture.md components wired)
- **Version:** N/A
- **Type:** Audio Effect (Multi-Distortion/Glitch)
- **Complexity:** 5.0 (capped) / 19.0 (uncapped raw score) — see `.ideas/plan.md`

## Lifecycle Timeline

- **2026-09-01:** Creative brief finalized via `/dream` — plugin-ideation skill
- **2026-09-01 (Stage 0):** Research & Planning complete — DSP architecture documented (14 core components across distortion, glitch, sequencer, mod matrix, macros, feedback routing, oversampling, limiter), complexity assessed (5.0 capped / 19.0 uncapped). Recommendation: split into Wave 1 (MVP-of-MVP) and Wave 2 (full feature set) implementation efforts rather than one continuous phased pass — see `.ideas/plan.md`.
- **2026-09-02:** UI mockup finalized (v5) and `parameter-spec.md` locked at v2 (94 parameters — the 56 mockup-bound v1 parameters plus 38 architecture-required additions: per-LFO shape/sync x4 and the 8-slot Mod Matrix's source/destination/amount/enable fields).
- **2026-09-02 (Stage 1):** Foundation + Shell complete — CMakeLists.txt (VST3/AU/Standalone) and full APVTS with all 94 locked parameters implemented in exact spec order, zero drift. `processBlock()` is an intentional pass-through stub; Stage 2 DSP owns all processing.
- **2026-09-03/04 (Stage 2):** DSP Implementation complete — all 9 phased sub-phases (3.1-3.9) delivered: Feedback Routing safety validation + integration, the Unified Modulation Accumulator, the core 12-algorithm Distortion/Bitcrush/7-topology Filter/Master Mix/dual-mode Limiter chain, the 18-mode Glitch/Buffer Engine (Tier 1 + Tier 2), the host-synced Rhythmic Sequencer (10 lanes, 10 pattern operations), the 8-slot Mod Matrix + 4 LFOs (9 shapes each) + 8-macro Macro System, and finally the XY Pad (with inertia), 7 MIDI-triggerable Performance Mode triggers, and the ECO/NORMAL/HIGH/EXTREME/AUTO Oversampling/Quality Engine (per-sample `juce::dsp::Oversampling` calls around Distortion+Bitcrush, preserving the Feedback Routing Path's sample-accurate causality). All 14 architecture.md Core Components now have real, wired DSP. Build/pluginval verification remains the orchestrator's `build-automation` follow-up (no dsp-agent session in this build had shell/execution tool access). Next: Stage 3 GUI (WebView UI, 6 phased sub-phases per `.ideas/plan.md`).

## Known Issues

None

## Additional Notes

Multi-type distortion/glitch "playground" effect combining 12 distortion algorithms (MVP), a real-time glitch/buffer engine, a host-synced 16/32-step rhythmic sequencer, a full modulation matrix with 4 LFOs, 8 macro controls, an XY pad, and Performance Mode live-triggers. Sonic/UX lineage: iZotope Trash / Soundtoys Decapitator. MVP scope is already larger than a typical first plugin — see `.ideas/creative-brief.md` Technical Notes for the full scope breakdown and what's deferred to post-MVP (multiband, parallel distortion, Euclidean rhythms, scenes, preset morphing, granular/frequency-shifter pitch FX).

Stage 0 research flagged this MVP as unusually large (uncapped complexity score 19.0, ~6x the formula's cap) — comparable in scope to a small commercial product (iZotope Trash 2 / Sugar Bytes Effectrix tier) rather than a typical single-pass plugin. Highest-risk component: the DISTORTION→FILTER→DELAY→DISTORTION feedback routing path (safety-critical, must be validated in isolation before other components depend on it — see `.ideas/architecture.md` Implementation Risks). See `.ideas/plan.md` for the full 9-phase DSP + 6-phase GUI breakdown and the recommendation to consider a Wave 1/Wave 2 scope split.
