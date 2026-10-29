# TENSION - Implementation Plan

**Date:** 2026-10-29
complexity_score: 5.0
**Complexity Score:** 5.0 (Complex)
**Strategy:** Phase-based implementation
**Complexity Tier:** 6 (DEEP research)

---

## Complexity Factors

- **Parameters:** 5 parameters (5/5 = 1.0 points, cap 2.0) = 1.0
  - tension, mix, output, sync, riseMode
- **Algorithms:** 10 DSP components = 10
  - TensionEngine, TensionMapping, LFOEngine, NoiseGenerator, FilterEngine, StereoWidth, ReverbEngine, DelayEngine, SaturationEngine, OutputSafety
- **Features:** 3 points
  - Feedback loops (+1): ping-pong delay and reverb network
  - Modulation systems (+1): two LFO instances, five shapes, host sync
  - Multiband processing (+1): LR4 150 Hz split in StereoWidth
  - (No FFT, no MIDI: 0)
- **Total:** 14.0 raw -> **5.0 (capped)**

Note: the cap means the score cannot distinguish TENSION's relative risk. Real risk sits in creative tuning (TensionMapping) rather than in algorithmic difficulty; every module is individually LOW-MEDIUM.

---

## Stages

- Stage 0: Research and Planning (architecture.md + plan.md) done
- Stage 1: Foundation + Shell (CMake, APVTS, bus layout, transparent passthrough) <- Next
- Stage 2/3: DSP (phased, below)
- Stage 4: GUI (phased, native JUCE)
- Stage 5: Validation (presets, pluginval, changelog)

Mapping to the spec's section 9 phases: spec P1 = Stage 0 (done); P2 = Stage 1; P3-P4 = DSP Phases 3.1-3.5; P5 = GUI Phases 4.1-4.3; P6-P7 = DSP Phase 3.6 + Validation.

---

## Complex Implementation (Score >= 3.0)

### Stage 3: DSP Phases

#### Phase 3.1: Engine Skeleton and Transparent Routing

**Goal:** TensionEngine, control-rate sub-blocking, smoothers, dry/proc/wet bus with engagement `e`, Mix, OutputSafety (scrub + knee), Output trim, mono/stereo handling. All processors are identity stubs.

**Components:** TensionEngine (skeleton), OutputSafety, host transport struct, telemetry atomics, console test harness.

**Test Criteria:**
- [ ] Zero-tension bit-exact passthrough (memcmp) for block sizes {1,32,512,4096}, rates {44.1k..192k}, mono and stereo
- [ ] Output trim and mix behave; no zipper on automation
- [ ] NaN/Inf injection yields finite output
- [ ] No allocation in processBlock (guard test)
- [ ] Block-size invariance

---

#### Phase 3.2: Core Series Modules

**Goal:** LFOEngine, FilterEngine, SaturationEngine, StereoWidth driven by a temporary linear stub mapping.

**Components:** LFOEngine (5 shapes, free + sync, phase continuity, deterministic S&H), FilterEngine (HP + BP sweep, Q cap), SaturationEngine (ADAA tanh, level compensation), StereoWidth (LR4 + M/S + rotation).

**Test Criteria:**
- [ ] LFO shape ranges/period accuracy; sync rate equals bpm-scaled value; no phase jump on rate change
- [ ] Filter stable at extremes (Q cap, cutoff clamp), cutoff sweep click-free
- [ ] Saturation: alias-bin energy >40 dB below fundamental at max drive; loudness change <1.5 dB over drive range; no DC
- [ ] Width: LR4 recombination flat at width 1; mono-sum loss <=3 dB at max width; mid never attenuated >1 dB

---

#### Phase 3.3: Parallel Sends and Noise

**Goal:** NoiseGenerator, ReverbEngine, DelayEngine with send/return and asymmetric smoothing.

**Components:** NoiseGenerator (xorshift32, pink/white/filtered, riser filters), ReverbEngine (juce::dsp::Reverb + predelay + send HPF/return LPF), DelayEngine (Lagrange3rd ping-pong, feedback HP/LP/tanh, time slew, hard cap 0.85).

**Test Criteria:**
- [ ] Silent input + tension ramp produces noise; silent at tension 0; deterministic golden hash with fixed seed
- [ ] Noise RMS monotone in tension; brightness (spectral centroid) rises
- [ ] Reverb tail rings out after tension drops (no gating); RT60 scales with roomSize; finite after parameter sweeps
- [ ] Delay: ping-pong alternation verified; 60 s max-feedback soak stable (peak bounded); tempo jump click-free

---

#### Phase 3.4: TensionMapping and Rise Modes

**Goal:** Replace the stub with the real nonlinear mapping: all destination curves, three rise-mode profiles (constexpr tables), mode crossfade, integration of all modules in the full chain.

**Components:** TensionMapping (TensionTargets struct, curve primitives), profile tables, modeBlend, per-destination smoothing times.

**Test Criteria:**
- [ ] Every curve monotonic in tension; stage onsets match spec (noise emerges ~25%, feedback rises ~75%, width/reverb/delay atmospheric ~60%)
- [ ] Rise modes differ measurably (noise RMS @50%, LFO rate @75%, saturation THD @100% each differ by >=2 dB / 25%)
- [ ] Switching mode during a ramp is click-free (600 ms blend)
- [ ] Full chain finite for sweep 0->1->0 in all modes; no gain jump larger than 3 dB per 100 ms during linear ramp

---

#### Phase 3.5: Musical Tuning and Metric Harness

**Goal:** Tune constants for 4/8/16/32-bar rises; offline metric renders (loudness vs t, centroid, width, RT60 estimate); optional level-compensator if cumulative gain >3 dB; decide Freeverb vs FDN upgrade.

**Components:** render/metric tool, constants refinement, optional FDN fallback behind ReverbEngine interface.

**Test Criteria:**
- [ ] Wet-bus loudness at t=1 stays within +6 dB of input RMS for reference material
- [ ] Listening review of 4/8/16/32-bar linear ramps at 120 BPM
- [ ] Reverb does not mask source (HP/LP send/return verified by spectrum)

---

#### Phase 3.6: Robustness and Soak

**Goal:** Spec phase 6 hardening: sample-rate matrix, block sizes, mono/stereo, state restore, denormal soak, runaway guard behavior, tail length reporting.

**Test Criteria:**
- [ ] Sample rates {44.1, 48, 88.2, 96, 176.4, 192} kHz all pass
- [ ] 60 s soak per mode at max settings finite and bounded
- [ ] State save/restore round-trip yields identical render
- [ ] Runaway guard engages under synthetic overload and recovers
- [ ] CPU <15% of one core at 48 kHz stereo (target)

---

### Stage 4: GUI Phases (native JUCE - see architecture.md decision; user to confirm)

#### Phase 4.1: Layout and LookAndFeel

**Goal:** CustomLookAndFeel (near-black gradient, restrained orange/red accents, clean typography), oversized TensionKnob component, compact MIX / OUTPUT / SYNC / RISE MODE controls, fixed layout.

**Test Criteria:**
- [ ] Editor opens at the designed size; TENSION knob visually dominant
- [ ] All controls rendered and styled correctly
- [ ] No WebView resources or NEEDS_WEB_BROWSER needed

---

#### Phase 4.2: Parameter Binding and Interaction

**Goal:** SliderParameterAttachment / ButtonParameterAttachment / ComboBoxParameterAttachment for all five parameters; gesture bracketing, double-click reset, fine-adjust modifier, percent/dB text.

**Test Criteria:**
- [ ] Control movement changes DSP; host automation moves the controls
- [ ] State restore updates UI
- [ ] Touch/latch automation records in the DAW (native Slider brackets gestures)

---

#### Phase 4.3: Reactive Circular Visualizer

**Goal:** Visualizer component driven by lock-free telemetry (tension, output peak, wet RMS, LFO phase) via a 60 Hz timer, fully independent of audio processing.

**Test Criteria:**
- [ ] Visualizer reacts to tension and signal level, smooth ballistics (fast attack / slow decay)
- [ ] No CPU spike; no audio-thread interaction beyond atomics
- [ ] Standalone launches; screenshot review (scoped to the plugin window)

---

### Implementation Flow

- Stage 1: Foundation + Shell - CMake (VST3 + AU + Standalone), APVTS (5 params), bus layout, passthrough
- Stage 3: DSP - 6 phases (3.1-3.6)
- Stage 4: GUI - 3 phases (4.1-4.3)
- Stage 5: Validation - presets, pluginval, changelog, install

---

## Implementation Notes

### Thread Safety
- Parameter reads via `getRawParameterValue()->load()`; telemetry via `std::atomic<float>`; no locks. State reset after safety event happens at block boundary on the audio thread.

### Performance
- Target <15% single core at 48 kHz stereo; reverb largest, ADAA log/cosh second. Control-rate work every 32 samples.

### Latency
- 0 samples. Tail length reported as 8 s.

### Denormal Protection
- `juce::ScopedNoDenormals`; feedback write flush `|x|<1e-20`.

### Known Challenges
- Musical tuning across 4-32 bars (largest risk). Mitigate with constexpr profile tables and the Phase 3.5 metric harness.
- Do not use `juce::dsp::Limiter` (hidden 4:1 compressor); OutputSafety is custom.
- Test harness: call `setPlayConfigDetails` before `prepareToPlay` (else sampleRate=0 and silent NaN renders).
- Bit-exact transparency depends on exact-zero snapping of smoothers at tension 0 and keeping the dry copy untouched.
- Context7 MCP was unavailable during Stage 0: verify JUCE 8 signatures (SVF TPT, LinkwitzRileyFilter, DelayLine, Reverb, `AudioPlayHead::getPosition`) when writing Stage 1/3 code.
- Reuse from DROP: send/return reverb/delay wrapper, noise seeding/dB-linear curves, scrub idea (read plugins/DROP/Source/DSP; do not modify DROP).
- Installing: product name "TENSION"; avoid renaming later (Pattern #14).

---

## References

- Creative brief: `plugins/TENSION/.ideas/creative-brief.md`
- Authoritative spec: `plugins/TENSION/.ideas/implementation-spec.md`
- Parameter spec: `plugins/TENSION/.ideas/parameter-spec-draft.md`
- DSP architecture: `plugins/TENSION/.ideas/architecture.md`
- UI mockup: none (native JUCE GUI recommended; a native design brief replaces the HTML mockup)

Similar plugins for reference:

- DROP - Noise/Reverb/Delay/Width/Saturation/Filter modules, send/return, test harness
- KICKR / PRISM - console test harness (`RunTests.cpp`) and native-GUI conventions
