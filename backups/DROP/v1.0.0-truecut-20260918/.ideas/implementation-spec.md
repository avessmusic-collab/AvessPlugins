# DROP — Full Implementation Specification

> Authoritative spec provided by the user on 2026-09-13. Stage 0 planning (architecture.md / plan.md) and all implementation stages must honor this document. The creative brief is a summary of this file.

Build a production-quality VST3 audio plugin called DROP. DROP is NOT a generic multi-FX rack. Its identity is a dedicated musical transition engine that turns a normal musical passage into BUILD → TENSION → VACUUM → IMPACT → RECOVERY. The core promise: "Give me a boring loop, tell me where the drop happens, and DROP creates the tension before it and the impact at the drop."

## CORE CONCEPT

Architecture: Input → musical analysis → TransitionEngine → IntensityBudget → coordinated DSP → Vacuum → ImpactEngine → Recovery. All effects must be coordinated musically over time instead of simply running independently. The key product differentiator is IntensityBudget: total effect intensity is distributed across modules so the transition sounds intentional rather than like every effect is maxed simultaneously.

## USER WORKFLOW

Insert DROP on a track or bus. User chooses transition length: 1/2/4/8/16 bars. User chooses Style, Intensity, Tension, Chaos, Impact and Dry/Wet. User triggers DROP manually, via MIDI, or host automation. DROP calculates a deterministic transition synchronized to tempo/transport. Example progression: subtle filtering → stereo narrowing → rhythmic gating → pitch movement → stutter → increasing delay/distortion → severe tension → short vacuum → clean/original transient impact → stereo expansion → short tail/recovery.

## UI

Minimal creative-instrument interface. Large DROP trigger button. Visual transition graph showing tension over time. Main controls: BUILD, STYLE, INTENSITY, TENSION, CHAOS, IMPACT, DRY/WET, OUTPUT. Advanced controls: Filter, Width, Pitch, Gate, Stutter, Delay, Reverb, Distortion, Digital, Noise. Do not expose hundreds of DSP parameters. UI must never execute DSP.

## TRIGGER MODES

Manual trigger. MIDI trigger with configurable MIDI note. Host automation Trigger parameter. Optional auto-sync mode. Trigger must be represented internally as deterministic sample-accurate events.

## MUSICALCLOCK

Create MusicalClock class containing BPM, PPQ, time signature, bar position, beat position, musical phase and sample-rate conversion. Support musical divisions 1/1, 1/2, 1/4, 1/8, 1/16, 1/32 plus triplets. Convert musical positions/durations to exact sample positions. Correctly handle tempo changes, sample-rate changes, variable block sizes, transport start/stop, looping and offline rendering. Never depend on wall-clock time.

## TRANSITIONENGINE

Create TransitionEngine that maps transition progress to a normalized 0–1 position. Support Linear, Exponential, S-Curve, LateAcceleration and Custom curves. Most tension should occur near the end by default. TransitionEngine must output normalized progression used by all DSP modules.

## STATE MACHINE

Implement explicit enum state machine: Idle, Arming, Building, Peak, Impact, Recovery. State transitions must be deterministic and sample-accurate. Prevent accidental retriggers and undefined intermediate states.

## AUDIOANALYZER

Create lightweight real-time analysis: RMS, peak, transient energy, spectral centroid, low/mid/high energy and stereo correlation. Smooth measurements. Avoid ML in MVP. Analysis must be efficient and real-time safe. Use analysis only to make processing source-aware and musical.

## SOURCE-AWARE PROCESSING

Adapt effect intensity to source characteristics. Vocals: favor filter, delay, pitch, reverb and width while avoiding aggressive destruction. Drums: favor gate, stutter, filter and saturation. Bass: favor filter/distortion/pitch while protecting sub frequencies. Full mix: conservative processing and strong transient/low-frequency protection.

## INTENSITYBUDGET

Implement central IntensityBudget. Total available intensity is normalized to 1.0 and distributed between filter, width, pitch, gate, stutter, delay, reverb, distortion, digital destruction and noise. Distribution changes with Style and transition progress. Do NOT let every effect reach maximum simultaneously. Style data should define weights and allowed behaviors. Budget allocation must be deterministic.

## FILTERMODULE

Support HP, LP, BP and dual filtering. Parameters: cutoff, resonance, slope and mix. Use stable filters. Make cutoff progression musical. During normal builds gradually remove or reshape frequencies. At high tension, allow stronger filtering. Adapt limits according to source. Protect against unstable resonance.

## WIDTHMODULE

Use Mid/Side processing. Narrow stereo during build and expand strongly at impact. Protect low frequencies from destructive width processing. Do not produce excessive phase cancellation.

## PITCHMODULE

Modes: Rise, Fall, Rise+Fall. Musical ranges: 0, +1, +2, +3, +5, +7, +12 semitones and reasonable downward equivalents where supported. Use suitable granular/phase-vocoder style pitch processing rather than naive resampling. Pitch amount must be transition-controlled and musically synchronized.

## GATEMODULE

Tempo-synced rhythmic gate. Rates: 1/2, 1/4, 1/8, 1/16, 1/32 plus triplets. Patterns: straight, triplet, accelerating and decelerating. Rate/pattern intensity increases toward the drop. Preserve musical transients when appropriate.

## STUTTERMODULE

Use internal audio buffer for short repeats. Lengths from 1/4 through 1/64. Modes: repeat, reverse, half, double and pitch variation. Stutter becomes more active toward the end of the transition. Avoid discontinuities and clicks. Preallocate buffers.

## DELAYMODULE

Tempo-synced stereo delay. Parameters: time, feedback, HPF, LPF, stereo offset, saturation and mix. Use interpolated delay where necessary. Prevent runaway feedback. Delay should become increasingly rhythmic as tension rises.

## REVERBMODULE

Low-CPU stereo reverb. Primarily used for impact and recovery. Parameters: size, decay, damping, width and mix. Avoid excessive CPU and avoid turning the impact into an indistinct wash.

## SATURATIONMODULE

Modes: Clean, Warm, Driven, Distorted, Crushed. Parameters: drive, tone and mix. Use selective 2x/4x oversampling only where useful. Saturation must remain stable and controlled.

## DIGITALMODULE

Bit-depth reduction and sample-rate reduction. Mostly active at high tension/high Chaos. Digital destruction should be a controlled accent, not the default sound.

## NOISEMODULE

White/pink/filtered noise. Parameters: amount, brightness, rise and stereo. Use as supplementary energy, not as a replacement for musical content.

## VACUUMMODULE

Critical signature feature. Immediately before impact, create a very short near-silence or energy collapse. Supported durations approximately 1/64–1/16. Can reduce overall gain, low-frequency content, high-frequency content and stereo width. Must be click-free and sample-accurate. Vacuum creates contrast before the impact.

## IMPACTENGINE

Impact should emphasize the original musical transient rather than simply becoming a huge processed sound. Optional short sub impact, transient/noise burst, controlled stereo expansion, saturation and short reverb/tail. Sub impact target approximately 40–80 Hz with fast attack and short decay. User can control impact strength. Preserve original source transient. Avoid muddy oversized low-end.

## IMPACT PHILOSOPHY

Default sequence: BUILD → TENSION → VACUUM → ORIGINAL/CLEAN IMPACT → SHORT TAIL → RECOVERY. The most important contrast is the disappearance immediately before the return. Do not make the impact a giant pile of effects.

## TRANSIENT PRESERVATION

Detect transient energy and avoid destroying the main source transient. During high tension, destructive processing may affect surrounding audio but the actual drop event should remain recognizable and punchy.

## LOW-FREQUENCY PROTECTION

Protect sub and bass content, especially below roughly 100 Hz, unless a deliberate effect explicitly targets it. Avoid phase problems, uncontrolled resonance and excessive sub buildup. Impact low-end must be controlled.

## DROPSTYLE

Create data-driven DropStyle structure containing effect weights for filter, width, pitch, gate, stutter, delay, reverb, distortion, digital, noise and impact. Include curve type, flags, timing preferences, source-awareness behavior and low-frequency rules. Do not hard-code large switch statements throughout DSP.

## CHAOS

Implement deterministic randomness using std::mt19937 or equivalent. Save the random seed in plugin state. Same preset + seed + source audio + transport must reproduce the same output. NEW/Randomize generates a new seed. Randomization must choose musically valid values, e.g. 1/8 delay, 1/16 gate, +5 semitones, rather than arbitrary nonsense. Chaos changes variation amount, not whether the result remains musical.

## CUSTOM TENSION CURVE

Allow user-defined normalized 0–1 curve. Use smooth interpolation. Clamp values safely. Curve must be serializable and restored exactly.

## VISUALIZATION

Display input level, transition position, tension amount, active effects and drop point. Use safe communication between audio thread and GUI. No locks, allocations or DSP inside paint callbacks. UI should represent the processor state, not own it.

## AUTOMATION PARAMETERS

Expose only high-value parameters: Trigger, Build Length, Style, Intensity, Tension, Chaos, Impact, Filter, Width, Pitch, Gate, Stutter, Delay, Reverb, Distortion, Noise, Dry/Wet, Output.

Use JUCE AudioProcessorValueTreeState for parameter management and state serialization.

## PARAMETER SMOOTHING

Smooth gain, mix, filter cutoff/resonance, drive, width, feedback and pitch-related continuous controls where necessary. Do not smooth intentionally rhythmic events in a way that destroys timing. Avoid zipper noise and clicks.

## REAL-TIME SAFETY

processBlock must not allocate memory, take locks, access filesystem, log, call GUI methods or dynamically resize containers. Preallocate all buffers during prepareToPlay. Support 44.1, 48, 88.2, 96 and 192 kHz. Support mono and stereo. Use denormal protection. Check for NaN/Inf during development/testing. Prevent runaway feedback and undefined DSP states.

## LATENCY

Prefer zero-latency MVP. If any module requires latency, report it correctly to the host and keep the behavior deterministic.

## SAMPLE-ACCURATE EVENTS

Impact, vacuum, gate, stutter and MIDI-triggered transitions must occur at correct sample positions. When event timing falls inside a processing block, process the block in segments or use equivalent sample-offset logic.

## OFFLINE RENDERING

Offline output must be deterministic. Never depend on GUI timers, system time or random wall-clock values. Handle offline rendering correctly.

## PLUGIN STATE

Save/restore all important state using JUCE AudioProcessorValueTreeState and appropriate ValueTree data. Persist parameters, selected Style, random seed, custom tension curve, trigger mode, MIDI trigger settings and required UI state.

## PRESETS

Create at least 16 useful presets: Clean EDM, Big Room, Techno Pressure, Dark Techno, Trap Pull, DnB Rush, Hyper, Cinematic, Minimal, Vocal Build, Drum Build, Synth Build, Full Mix, Destroy, Vacuum, Reverse Energy.

Presets must demonstrate genuinely different transition behavior rather than only different parameter values.

## AUDIO QUALITY

Use stable filters. Use proper interpolated delay where needed. Prevent unsafe feedback. Use DC blocking where appropriate. Handle denormals. Maintain correct stereo behavior. Use oversampling selectively. Any aliasing introduced intentionally by Digital mode should be controlled and musically useful.

## PROJECT ARCHITECTURE

Adapt to the existing repository rather than blindly replacing working infrastructure.

Suggested structure:

```
Source/PluginProcessor.*
Source/PluginEditor.*
Source/DSP/MusicalClock.*
Source/DSP/TransitionEngine.*
Source/DSP/DropStateMachine.*
Source/DSP/AudioAnalyzer.*
Source/DSP/FilterModule.*
Source/DSP/WidthModule.*
Source/DSP/PitchModule.*
Source/DSP/GateModule.*
Source/DSP/StutterModule.*
Source/DSP/DelayModule.*
Source/DSP/ReverbModule.*
Source/DSP/SaturationModule.*
Source/DSP/DigitalModule.*
Source/DSP/NoiseModule.*
Source/DSP/VacuumModule.*
Source/DSP/ImpactEngine.*
Source/DSP/IntensityBudget.*
Source/DSP/ParameterSmoother.*
Source/Core/DropStyle.*
Source/Core/DropPreset.*
Source/Core/DropRandomizer.*
Source/UI/DropEditor.*
Source/UI/DropGraph.*
Source/UI/DropButton.*
Source/UI/MeterDisplay.*
Source/UI/ParameterControl.*
```

Use C++20, JUCE and CMake. Build VST3 and a standalone test application. Avoid unnecessary dependencies.

## DEVELOPMENT PROCESS

- Phase 1: Inspect repository. Determine existing JUCE version, CMake setup, plugin formats, source structure and current DSP infrastructure. Do not overwrite working infrastructure. Create/complete plugin skeleton, parameters, state handling and build configuration.
- Phase 2: Implement MusicalClock and unit tests.
- Phase 3: Implement TransitionEngine and explicit state machine.
- Phase 4: Implement core DSP: filter, width, gate, saturation, delay and reverb.
- Phase 5: Implement creative DSP: stutter, pitch, digital, vacuum and noise.
- Phase 6: Implement ImpactEngine.
- Phase 7: Implement IntensityBudget, DropStyle and presets.
- Phase 8: Implement UI and visualization.
- Phase 9: Complete preset library and parameter behavior.
- Phase 10: Full validation, profiling and cleanup.

After every phase: compile, run tests, inspect warnings/errors, fix issues, verify state handling and only then continue.

## UNIT TESTS

- Test MusicalClock timing and musical-to-sample conversion.
- Test transition curves.
- Test state-machine transitions.
- Test deterministic randomization.
- Test filter stability.
- Test delay feedback safety.
- Process silence, DC, impulse, sine, noise and full-scale signals.
- Verify no NaN, Inf, runaway gain or unexpected DC buildup.
- Test parameter automation changes occurring inside a block.
- Verify no clicks and correct event timing.

## DAW TESTING

Test VST3 loading and validation. Test automation. Test MIDI trigger. Test transport start/stop. Test looping. Test tempo changes. Test sample-rate changes. Test mono/stereo. Test variable block sizes. Test offline rendering. When environments are available, test Ableton Live, REAPER, Logic Pro and Cubase.

## PERFORMANCE

Profile release build. Avoid excessive FFT usage, oversampling and large buffers. Keep analysis lightweight. CPU must remain appropriate for a real-time creative effect. Never trade timing correctness for unnecessary visual complexity.

## DESIGN PRIORITY

1. Timing
2. Musical tension
3. Contrast
4. Impact
5. Audio quality
6. CPU efficiency
7. UI simplicity
8. Feature count

## DEFINITION OF DONE

A producer can insert DROP, select a 1/2/4/8/16-bar transition, choose a Style, adjust Intensity/Tension/Chaos/Impact, press DROP and immediately hear a convincing build, tension rise, vacuum, impact and recovery. The same transition remains synchronized to tempo, automation works correctly, MIDI triggering works, presets load correctly, state saves/reloads correctly and offline renders are deterministic.

## IMPORTANT IMPLEMENTATION RULES

- Do not turn DROP into an ordinary Effect Rack.
- Do not create dozens of unrelated controls just because they are technically possible.
- Do not let every effect operate at maximum simultaneously.
- Do not sacrifice sample accuracy for convenience.
- Do not use random values without musical constraints.
- Do not create clicks at transition boundaries.
- Do not destroy all low end during normal builds.
- Do not make the impact depend entirely on synthesized sounds; preserve the source transient.
- Do not use GUI state as the source of truth.
- Do not allocate or lock in the audio thread.
- Do not introduce unnecessary third-party dependencies.

## FIRST ACTION

Inspect the repository and identify the current project structure, build system, JUCE version, plugin target and existing DSP. Then produce a concise implementation plan based on the actual repository and begin Phase 1 only. Build and test Phase 1, fix issues, then continue phase-by-phase. Make sensible engineering decisions without asking unnecessary questions. The final implementation should feel like a professional musical instrument, not a collection of effects.
