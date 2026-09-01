# CORRUPTR Notes

## Status
- **Current Status:** 🚧 Stage 0 (Research & Planning Complete)
- **Version:** N/A
- **Type:** Audio Effect (Multi-Distortion/Glitch)
- **Complexity:** 5.0 (capped) / 19.0 (uncapped raw score) — see `.ideas/plan.md`

## Lifecycle Timeline

- **2026-09-01:** Creative brief finalized via `/dream` — plugin-ideation skill
- **2026-09-01 (Stage 0):** Research & Planning complete — DSP architecture documented (14 core components across distortion, glitch, sequencer, mod matrix, macros, feedback routing, oversampling, limiter), complexity assessed (5.0 capped / 19.0 uncapped). Recommendation: split into Wave 1 (MVP-of-MVP) and Wave 2 (full feature set) implementation efforts rather than one continuous phased pass — see `.ideas/plan.md`.

## Known Issues

None

## Additional Notes

Multi-type distortion/glitch "playground" effect combining 12 distortion algorithms (MVP), a real-time glitch/buffer engine, a host-synced 16/32-step rhythmic sequencer, a full modulation matrix with 4 LFOs, 8 macro controls, an XY pad, and Performance Mode live-triggers. Sonic/UX lineage: iZotope Trash / Soundtoys Decapitator. MVP scope is already larger than a typical first plugin — see `.ideas/creative-brief.md` Technical Notes for the full scope breakdown and what's deferred to post-MVP (multiband, parallel distortion, Euclidean rhythms, scenes, preset morphing, granular/frequency-shifter pitch FX).

Stage 0 research flagged this MVP as unusually large (uncapped complexity score 19.0, ~6x the formula's cap) — comparable in scope to a small commercial product (iZotope Trash 2 / Sugar Bytes Effectrix tier) rather than a typical single-pass plugin. Highest-risk component: the DISTORTION→FILTER→DELAY→DISTORTION feedback routing path (safety-critical, must be validated in isolation before other components depend on it — see `.ideas/architecture.md` Implementation Risks). See `.ideas/plan.md` for the full 9-phase DSP + 6-phase GUI breakdown and the recommendation to consider a Wave 1/Wave 2 scope split.
