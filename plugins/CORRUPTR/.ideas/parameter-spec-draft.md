# Parameter Specification (Draft)

**Status:** Draft - Awaiting UI mockup for full specification
**Created:** 2026-09-01
**Source:** Quick capture during ideation (drafted from creative-brief.md, MVP scope only)

This is a lightweight specification to enable parallel DSP research.
Full specification will be generated from finalized UI mockup.

Scope note: this covers MVP-confirmed features only (12 distortion algorithms, glitch, 16/32-step host-synced sequencer, 4-LFO mod matrix, 8 macros, XY pad, Performance Mode, feedback path, filters, output limiter). Multiband, parallel distortion, Euclidean rhythms, scenes, and preset morphing are post-MVP and excluded here.

## Parameters

### inputGain
- **Type:** Float
- **Range:** -24 to +24 dB
- **Default:** 0 dB
- **DSP Purpose:** Input gain staging before the processing chain.

### distortionAlgorithm
- **Type:** Choice
- **Choices:** Saturation, Soft Clip, Hard Clip, Tube, Diode, Fuzz, Digital Distortion, Bitcrusher, Sample Rate Reducer, Wavefolder, Ring-Mod, Foldback
- **Default:** Saturation
- **DSP Purpose:** Selects the active waveshaping algorithm in the main distortion stage.

### drive
- **Type:** Float
- **Range:** 0 to 40 dB
- **Default:** 6 dB
- **DSP Purpose:** Master drive/gain feeding the selected distortion algorithm's waveshaper.

### tone
- **Type:** Float
- **Range:** -100 to +100 (dark to bright)
- **Default:** 0
- **DSP Purpose:** Tilts the tonal balance of the distortion stage output.

### bias
- **Type:** Float
- **Range:** -100 to +100
- **Default:** 0
- **DSP Purpose:** Asymmetry control for asymmetric/diode/tube-style clipping.

### fold
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Wavefolder fold amount (active for Wavefolder/Foldback algorithms).

### distortionMix
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 100%
- **DSP Purpose:** Dry/wet blend for the distortion stage specifically.

### bitDepth
- **Type:** Float
- **Range:** 1 to 16 bits
- **Default:** 16 bits
- **DSP Purpose:** Bitcrusher resolution reduction.

### sampleRateReduction
- **Type:** Float
- **Range:** 1 to 48 (downsample factor)
- **Default:** 1 (no reduction)
- **DSP Purpose:** Sample-and-hold style sample-rate reduction amount.

### filterType
- **Type:** Choice
- **Choices:** LP, HP, BP, Notch, Comb, Resonant LP, Resonant HP
- **Default:** LP
- **DSP Purpose:** Selects filter topology for the pre/post filter stage.

### filterCutoff
- **Type:** Float
- **Range:** 20 Hz to 20000 Hz (logarithmic)
- **Default:** 20000 Hz
- **DSP Purpose:** Filter cutoff frequency.

### filterResonance
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Filter resonance/emphasis at cutoff.

### glitchMode
- **Type:** Choice
- **Choices:** Off, Stutter, Repeat, Reverse, Buffer Repeat, Freeze, Slice, Random Slice, Micro Loop, Granular Repeat, Tape Stop, Tape Start, Retrigger, Silence, Noise Burst, Bitcrush Burst, Pitch Jump, Random Repeat
- **Default:** Off
- **DSP Purpose:** Selects the active glitch/buffer-manipulation mode.

### glitchBufferLength
- **Type:** Choice
- **Choices:** 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars
- **Default:** 1/16
- **DSP Purpose:** Musical division controlling the glitch engine's circular buffer window length.

### glitchProbability
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Per-event probability that a glitch event triggers.

### chaos
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Global unpredictability control — 0% fully deterministic, 100% highly unpredictable but musically constrained; uses a deterministic random seed for preset repeatability.

### sequencerSteps
- **Type:** Choice
- **Choices:** 16, 32
- **Default:** 16
- **DSP Purpose:** Number of steps in the rhythmic sequencer.

### sequencerRate
- **Type:** Choice
- **Choices:** 1/4, 1/8, 1/16, 1/32, 1/4T, 1/8T, 1/16T
- **Default:** 1/16
- **DSP Purpose:** Host-synced step rate (MVP: host-synced only, no free-run internal clock).

### feedbackAmount
- **Type:** Float
- **Range:** 0 to 100% (internally soft-clamped, cannot reach true unity/runaway)
- **Default:** 0%
- **DSP Purpose:** Feedback send level for the DISTORTION→FILTER→DELAY→DISTORTION path. Conservative auto-limiter prevents runaway/NaN/Inf regardless of setting.

### feedbackDamping
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 50%
- **DSP Purpose:** High-frequency damping applied inside the feedback loop.

### lfo1Rate through lfo4Rate
- **Type:** Float (with tempo-sync toggle per LFO)
- **Range:** 0.01 to 20 Hz (free) OR synced note divisions
- **Default:** 1 Hz, free-running
- **DSP Purpose:** Rate for each of the 4 freely-assignable LFOs (sine, triangle, saw, reverse saw, square, S&H, random, smooth random, random-walk shapes).

### macroDamage
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Combined macro increasing Drive + Clip + Fold + Feedback simultaneously.

### macroCrush
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Combined macro driving bitcrush/sample-rate-reduction-oriented parameters.

### macroGlitch
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Combined macro increasing glitch probability + buffer repetition + sample-rate reduction + pitch variation.

### macroChaos
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Combined macro driving randomization intensity across mapped destinations.

### macroRhythm
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Combined macro driving sequencer/rhythmic-gate intensity.

### macroMovement
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 0%
- **DSP Purpose:** Combined macro driving modulation depth/LFO intensity across mapped destinations.

### macroWidth
- **Type:** Float
- **Range:** 0 to 100%
- **Default:** 50%
- **DSP Purpose:** Combined macro driving stereo width/spread parameters.

### macroMix
- **Type:** Float
- **Range:** 0 to 200%
- **Default:** 100%
- **DSP Purpose:** Combined macro driving overall dry/wet mix (mirrors master Mix, can exceed 100% for parallel-boost feel).

### xyPadX / xyPadY
- **Type:** Float
- **Range:** 0 to 100% each axis
- **Default:** X=0% (mapped to macroDamage), Y=0% (mapped to macroGlitch)
- **DSP Purpose:** Performance XY pad; each axis modulates its assigned macro/parameter with smoothing/inertia (glide, not instant) to avoid zipper noise.

### xyPadSmoothing
- **Type:** Float
- **Range:** 0 to 500 ms
- **Default:** 80 ms
- **DSP Purpose:** Glide time for XY pad movement toward target values.

### performanceKill / performanceGlitch / performanceDestroy / performanceFreeze / performanceReverse / performanceStutter / performanceChaos
- **Type:** Bool (momentary or toggle, MIDI-triggerable)
- **Range:** Off/On
- **Default:** Off
- **DSP Purpose:** Live-performance trigger buttons; each engages a predefined combination of processing states for instant dramatic change.

### qualityMode
- **Type:** Choice
- **Choices:** ECO, NORMAL (2x), HIGH (4x), EXTREME (8x), AUTO
- **Default:** NORMAL (2x) — user-selectable, not hidden
- **DSP Purpose:** Oversampling factor applied around nonlinear stages to control aliasing; user chooses their own CPU/quality tradeoff.

### outputLimiterStyle
- **Type:** Choice
- **Choices:** Transparent, Colored
- **Default:** Transparent
- **DSP Purpose:** Final safety limiter character — Transparent is a pure peak-catcher, Colored adds gentle saturation when limiting engages.

### outputGain
- **Type:** Float
- **Range:** -24 to +24 dB
- **Default:** 0 dB
- **DSP Purpose:** Output gain staging after the limiter.

### mix
- **Type:** Float
- **Range:** 0 to 200%
- **Default:** 100%
- **DSP Purpose:** Master dry/wet; >100% allows the wet signal to dominate/overdrive the blend for a parallel-distortion feel.

## Next Steps

- [ ] Complete UI mockup workflow (/dream → option 3)
- [ ] Finalize design and generate full parameter-spec.md
- [ ] Validate consistency between draft and final spec
