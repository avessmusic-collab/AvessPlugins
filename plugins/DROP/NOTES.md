# DROP Notes

## Status
- **Current Status:** 🚧 Stage 1 (Foundation + Shell complete)
- **Version:** N/A
- **Type:** Audio Effect (Transition Engine)

## Lifecycle Timeline

- **2026-09-13:** Creative brief + authoritative implementation-spec.md supplied by user (Ideation)
- **2026-09-13:** Draft parameter spec derived from implementation spec (18 parameters, 2 open questions flagged)
- **2026-09-13 (Stage 0):** Research & Planning complete — architecture.md and plan.md documented (Complexity 5.0, phased implementation)
- **2026-09-14:** UI mockup finalized (v10 "Light Neumorphic / White Marble") — generated the LOCKED `parameter-spec.md` (19 parameters, `digital` added, module macros corrected to default 100%)
- **2026-09-15 (Stage 1):** Foundation + Shell complete — CMakeLists.txt (VST3/AU/Standalone, MIDI input, WebView flag pre-wired for Stage 3), full 19-parameter APVTS in `Source/PluginProcessor.{h,cpp}`, non-parameter ValueTree state (trigger mode, MIDI trigger note, Chaos seed, custom tension curve points, UI advanced-row flag), placeholder editor sized to the finalized mockup's dimensions, real-time-safe pass-through `processBlock()`. Build verification pending (build-automation skill runs next).

## Known Issues

None (pre-implementation).

## Additional Notes

**Description:**
DROP is a dedicated musical transition engine — BUILD → TENSION → VACUUM → IMPACT → RECOVERY — NOT a generic multi-FX rack. Central differentiator is IntensityBudget: total effect intensity normalized to 1.0 and distributed across 10 modules (Filter, Width, Pitch, Gate, Stutter, Delay, Reverb, Saturation/Distortion, Digital, Noise) by a data-driven DropStyle × transition progress, so a build sounds intentional rather than every effect maxed simultaneously. Signature moment: a very short, click-free, sample-accurate Vacuum immediately before an Impact that emphasizes the original source transient (not a synthesized wall of effect).

**Parameters (19 total, resolved from an 18-parameter draft during Stage 0):**
Trigger, Build Length (1/2/4/8/16 bars), Style (16 DropStyles), Intensity, Tension, Chaos, Impact, Filter, Width, Pitch, Gate, Stutter, Delay, Reverb, Distortion, **Digital** (added during Stage 0 — see architecture.md Resolved Open Questions), Noise, Dry/Wet, Output. The 10 module macros are unipolar 0-100% multipliers on Style-authored weight curves, default 100% (corrected from the draft's assumed 50% default during Stage 0).

**Core DSP (17 components, see architecture.md):**
MusicalClock, TransitionEngine, DropStateMachine, AudioAnalyzer, IntensityBudget, FilterModule, WidthModule, PitchModule (granular/WSOLA, not FFT phase vocoder — zero-latency-preferred + quantized semitone steps), GateModule, StutterModule, DelayModule, ReverbModule, SaturationModule, DigitalModule, NoiseModule, VacuumModule, ImpactEngine. Plus a data-driven Style/Preset/Chaos coordination layer (DropStyle, DropRandomizer, 16 presets).

**Complexity:** 5.0 (max, Tier 6) — 19 params (capped 2.0) + 17 algorithms + 4 features (FFT analysis, delay feedback loop, pervasive modulation, MIDI trigger). Phased implementation: 5 DSP phases + 3 GUI phases + Validation, mapped onto implementation-spec.md's own mandated 10-phase compile+test-gated development process.

**Preserved Artifacts:**
- Creative brief: `.ideas/creative-brief.md`
- Authoritative full spec: `.ideas/implementation-spec.md`
- Parameter spec (draft): `.ideas/parameter-spec-draft.md`
- DSP architecture: `.ideas/architecture.md`
- Implementation plan: `.ideas/plan.md`

**Next Steps:**
- build-automation: verify DROP compiles (VST3/AU/Standalone)
- Stage 3 DSP Phase 3.1 (Timing & Control Skeleton): MusicalClock, TransitionEngine, DropStateMachine, sample-accurate block segmentation, trigger/MIDI-note wiring
