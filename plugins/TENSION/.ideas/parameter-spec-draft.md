# Parameter Specification (Draft)

**Status:** Draft - derived directly from the authoritative implementation-spec.md (§7)
**Created:** 2026-10-29
**Source:** User-provided build spec (implementation-spec.md is authoritative)

This is a lightweight specification to enable Stage 0 DSP research.
The full specification will be generated when the UI is finalized.

## Parameters

### tension
- **Type:** Float
- **Range:** 0 to 100 % (normalized 0.0–1.0 internally)
- **Default:** 0 %
- **DSP Purpose:** Master macro. Drives ALL DSP destinations through independent nonlinear curves (TensionMapping): noise amplitude/brightness, LFO rate/depth, filter movement, stereo width, reverb wet/decay/depth, delay wet/feedback, saturation drive. 0% = bit-transparent passthrough. Also the effect's engagement/ride control — users automate this knob over 4–32 bars.

### mix
- **Type:** Float
- **Range:** 0 to 100 %
- **Default:** 100 %
- **DSP Purpose:** Global dry/wet balance over the entire processed signal (independent dry/wet routing per spec §3).

### output
- **Type:** Float
- **Range:** -24 to +12 dB
- **Default:** 0 dB
- **DSP Purpose:** Output trim applied after OutputSafety.

### sync
- **Type:** Bool
- **Default:** On (true)
- **DSP Purpose:** When on, LFOEngine rates and DelayEngine times lock to host BPM (musical divisions); when off, free-running Hz/ms equivalents.

### riseMode
- **Type:** Choice
- **Choices:** Slow Burn, Cinematic, Aggressive
- **Default:** Cinematic
- **DSP Purpose:** Selects the master mapping profile in TensionMapping. Each mode reshapes the actual per-destination tension curves (modulation speed, noise prominence, spectral movement, saturation) — not merely output gain.

## Next Steps

- [ ] Stage 0 research (architecture.md + plan.md)
- [ ] UI design → finalize full parameter-spec.md
- [ ] Validate consistency between draft and final spec
