# Parameter Specification (Draft)

**Status:** Draft - Awaiting UI mockup for full specification
**Created:** 2026-09-13
**Source:** Derived directly from the user's implementation spec (AUTOMATION PARAMETERS section of implementation-spec.md) — not interactively captured

This is a lightweight specification to enable parallel DSP research.
Full specification will be generated from finalized UI mockup.

All parameters managed via JUCE AudioProcessorValueTreeState. Per the spec, ONLY these high-value parameters are exposed; per-module DSP internals stay internal and are driven by DropStyle × IntensityBudget × transition progress.

## Parameters

### trigger
- **Type:** Bool (momentary trigger semantics)
- **Default:** Off (false)
- **DSP Purpose:** Fires the transition. Must be representable as a deterministic, sample-accurate internal event. Also triggerable via configurable MIDI note and host automation (trigger mode + MIDI note stored as non-parameter ValueTree state).

### buildLength
- **Type:** Choice
- **Choices:** 1 bar, 2 bars, 4 bars, 8 bars, 16 bars
- **Default:** 4 bars
- **DSP Purpose:** Total transition duration, tempo-synced via MusicalClock; converted to exact sample positions.

### style
- **Type:** Choice
- **Choices:** Clean EDM, Big Room, Techno Pressure, Dark Techno, Trap Pull, DnB Rush, Hyper, Cinematic, Minimal, Vocal Build, Drum Build, Synth Build, Full Mix, Destroy, Vacuum, Reverse Energy
- **Default:** Clean EDM
- **DSP Purpose:** Selects the data-driven DropStyle: effect weights, curve type, timing preferences, source-awareness behavior, low-frequency rules.

### intensity
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50
- **DSP Purpose:** Scales the overall IntensityBudget (total normalized to 1.0, distributed across the 10 modules).

### tension
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50
- **DSP Purpose:** Shapes the tension curve emphasis (default curves put most tension near the end of the build).

### chaos
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 0
- **DSP Purpose:** Amount of deterministic, seeded (std::mt19937), musically-quantized variation. Changes variation amount, never musicality. Seed persisted in state.

### impact
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50
- **DSP Purpose:** ImpactEngine strength: optional 40–80 Hz sub hit, transient/noise burst, stereo expansion, saturation, short tail. Source transient always preserved.

### filter
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral — scales Style-defined weight; Stage 0 to confirm)
- **DSP Purpose:** Macro weight for FilterModule (HP/LP/BP/dual) within the IntensityBudget.

### width
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral)
- **DSP Purpose:** Macro weight for WidthModule (M/S narrowing during build, expansion at impact, low-frequency protected).

### pitch
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral)
- **DSP Purpose:** Macro weight for PitchModule (Rise/Fall/Rise+Fall; musical semitone steps 0/+1/+2/+3/+5/+7/+12; granular/phase-vocoder style).

### gate
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral)
- **DSP Purpose:** Macro weight for GateModule (tempo-synced 1/2–1/32 + triplets; straight/triplet/accelerating/decelerating patterns).

### stutter
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral)
- **DSP Purpose:** Macro weight for StutterModule (preallocated buffer repeats 1/4–1/64; repeat/reverse/half/double/pitch modes).

### delay
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral)
- **DSP Purpose:** Macro weight for DelayModule (tempo-synced interpolated stereo delay, feedback-safe, HPF/LPF, stereo offset, saturation).

### reverb
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral)
- **DSP Purpose:** Macro weight for ReverbModule (low-CPU stereo; primarily impact tail and recovery).

### distortion
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral)
- **DSP Purpose:** Macro weight for SaturationModule (Clean/Warm/Driven/Distorted/Crushed; selective 2x/4x oversampling).

### noise
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 50 (neutral)
- **DSP Purpose:** Macro weight for NoiseModule (white/pink/filtered riser; supplementary energy only).

### dryWet
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 100
- **DSP Purpose:** Global dry/wet mix, smoothed.

### output
- **Type:** Float
- **Range:** -60 to +12 dB
- **Default:** 0 dB
- **DSP Purpose:** Output gain, smoothed.

## Open Questions for Stage 0

- **Digital module macro:** The spec's UI advanced-controls list includes "Digital" (bit/SR crush) but the AUTOMATION PARAMETERS list omits it. Decide: add a `digital` macro parameter (19th) or keep Digital internal (Chaos/Style-driven only).
- **Macro semantics/defaults:** Confirm whether module macros scale Style weights around a neutral 50% or act as absolute 0–100% weights (defaults above assume neutral scaling).
- **Non-parameter state (ValueTree):** trigger mode (manual/MIDI/auto-sync), MIDI trigger note, Chaos random seed, custom tension curve points, transition curve type, required UI state.

## Next Steps

- [ ] Complete UI mockup workflow (/dream → mockup option)
- [ ] Finalize design and generate full parameter-spec.md
- [ ] Validate consistency between draft and final spec
