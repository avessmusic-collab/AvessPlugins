# TENSION — Implementation Spec (AUTHORITATIVE)

> Provided verbatim by the user on 2026-10-29. This document is the authoritative
> contract for TENSION, superseding the creative brief where they differ.
> Factory amendments (deviations agreed during development) are listed at the end.

---

Build a complete, professional VST3 audio effect plugin called TENSION using C++20, JUCE and CMake. You are an expert C++ audio DSP engineer, JUCE developer and cinematic sound designer. Create actual working project files, not pseudocode or mockups. Compile, test and fix errors throughout development.

# 1. CONCEPT

TENSION is a one-knob cinematic tension/riser plugin. Users insert it on any audio track and automate one large TENSION knob (0–100%) to progressively transform the incoming audio into a cinematic riser. The knob simultaneously controls LFO modulation, procedural noise, filtering, stereo width, reverb, delay, feedback and saturation through coordinated nonlinear mappings.

At 0%, audio remains transparent. At 50%, it becomes atmospheric, wider and increasingly suspenseful. At 100%, it produces an intense cinematic build with rising noise, accelerating modulation, spectral movement and expansive spatial effects. It must also generate risers from silence using its internal noise generator.

# 2. TECHNICAL REQUIREMENTS

- C++20, JUCE, CMake.
- VST3 and Standalone targets.
- Windows/macOS compatibility.
- AudioProcessorValueTreeState for parameters and state management.
- Separate DSP, GUI and parameter mapping.
- No allocations, locks, file I/O or GUI operations inside processBlock().
- Allocate buffers during prepareToPlay().
- Support mono/stereo, variable buffer sizes and sample rates from 44.1–192 kHz.
- Smooth automation without zipper noise.
- Use current JUCE APIs and avoid deprecated implementations.

# 3. DSP ARCHITECTURE

Implement modular processing:

INPUT → Input Gain → Pre-Filter → Modulation → Saturation → Stereo Processing → Parallel Delay/Reverb → Output Safety → OUTPUT.

Implement procedural noise as an independent parallel signal generator with its own filtering, gain and modulation. Maintain independent dry/wet routing.

Create these modules:

**TensionEngine:** Main controller coordinating all DSP modules.

**TensionMapping:** Converts the master knob into nonlinear DSP parameter curves using smoothstep, exponential and musical mappings. Do not increase every effect linearly.

**LFOEngine:** Sine, triangle, saw, reverse saw and sample-and-hold. Support free-running and host BPM synchronization. Modulate amplitude, filtering, stereo movement and delay.

**NoiseGenerator:** White, pink and filtered noise. Noise progressively increases in volume and brightness as tension rises. Include rising high-pass/band-pass movement. Silence at 0% tension. Support deterministic generation for testing.

**FilterEngine:** Low-pass, high-pass and band-pass filtering with controllable resonance and smooth cutoff modulation. Avoid unstable resonance.

**StereoWidth:** Mid-side processing with progressive stereo expansion. Preserve center information, maintain low-frequency mono compatibility and avoid excessive phase cancellation.

**ReverbEngine:** Atmospheric reverb with tension-controlled wet level, decay, damping, pre-delay and spatial depth. Preserve natural tails and source definition.

**DelayEngine:** Stereo tempo-synced delay with ping-pong behavior, feedback filtering and smooth delay-time transitions. Prevent runaway feedback.

**SaturationEngine:** Musical soft saturation that increases perceived intensity without excessive distortion.

**OutputSafety:** Prevent NaNs, infinities, runaway feedback and uncontrolled peaks.

# 4. MASTER TENSION BEHAVIOR

The most important feature is the relationship between the knob and all DSP modules. Design a coordinated cinematic progression:

- 0%: Transparent original audio.
- 10%: Subtle movement.
- 25%: Noise begins emerging.
- 40%: Increasing modulation and spectral movement.
- 60%: Wider stereo image, atmospheric delay and reverb.
- 75%: Faster modulation, rising noise and increasing feedback.
- 90%: Intense cinematic build.
- 100%: Maximum controlled tension and climax.

These are conceptual stages, not discrete switching points. All transitions must be continuous and smooth.

As tension increases, progressively raise noise brightness, noise amplitude, modulation speed/depth, stereo width, spatial depth, harmonic intensity and spectral energy. Use independent nonlinear curves for each destination.

The plugin must sound musical when automating TENSION from 0–100% across 4, 8, 16 or 32 bars.

# 5. RISE MODES

Implement three selectable mapping profiles:

- **SLOW BURN:** Gradual atmospheric progression with restrained modulation and long spatial development.
- **CINEMATIC:** Balanced noise, filtering, width, reverb and delay progression.
- **AGGRESSIVE:** Faster modulation, stronger spectral movement, more pronounced noise and saturation.

Each mode must modify the actual DSP mapping, not simply the output gain.

# 6. GUI

Create a premium minimalist interface using native JUCE graphics and custom LookAndFeel.

Visual direction: Near-black background, subtle gradients, clean typography, restrained orange/red accents.

One oversized central circular knob labeled TENSION, displaying 0–100%.

Include a reactive circular intensity visualization.

Additional compact controls:
- MIX
- OUTPUT
- SYNC ON/OFF
- RISE MODE

Keep the central knob visually dominant. No unnecessary controls or visual clutter. GUI animations must remain independent of audio processing.

# 7. PARAMETERS

Expose stable automatable DAW parameters: TENSION, MIX, OUTPUT, SYNC and RISE MODE.

Use APVTS and proper parameter attachments. Implement complete state saving/restoration. Use suitable smoothing times for individual DSP destinations rather than one universal smoothing constant.

# 8. PROJECT STRUCTURE

```
Tension/
  CMakeLists.txt
  README.md
  CLAUDE.md
  Source/
    PluginProcessor.h/.cpp
    PluginEditor.h/.cpp
    DSP/
      TensionEngine.h/.cpp
      TensionMapping.h/.cpp
      LFOEngine.h/.cpp
      NoiseGenerator.h/.cpp
      FilterEngine.h/.cpp
      StereoWidth.h/.cpp
      ReverbEngine.h/.cpp
      DelayEngine.h/.cpp
      SaturationEngine.h/.cpp
      OutputSafety.h/.cpp
    UI/
      TensionKnob.h/.cpp
      CustomLookAndFeel.h/.cpp
      Visualizer.h/.cpp
    Tests/
  Presets/
```

Improve this structure if technically justified. Keep modules independent and testable.

# 9. DEVELOPMENT PROCESS

Work autonomously in these phases:

- PHASE 1: Inspect the development environment, compiler, JUCE installation and CMake. Research relevant JUCE APIs and establish the DSP architecture.
- PHASE 2: Generate the project, CMake configuration, AudioProcessor, AudioProcessorEditor, parameters and state serialization. Compile the foundation.
- PHASE 3: Implement individual DSP modules, test them and integrate the complete signal chain.
- PHASE 4: Implement the nonlinear master tension mapping and three rise modes.
- PHASE 5: Create the custom GUI and connect all controls to real parameters.
- PHASE 6: Test zero-tension transparency, silent-input generation, automation, sample-rate changes, mono/stereo compatibility, feedback stability, reverb tails, state restoration, variable block sizes and numerical stability.
- PHASE 7: Build Release, run available plugin validation tools, fix issues and document installation.

Compile after each major phase. Do not continue building on broken code.

# 10. SOUND DESIGN PRIORITIES

This must sound like a purpose-built cinematic tension instrument, not a generic multi-effects chain. Prioritize coordinated psychoacoustic progression, musical movement and smooth automation.

Avoid excessive noise, harsh resonance, uncontrolled feedback, stereo phase problems, volume jumps, clicks and reverb masking.

Preserve the original source's definition while progressively increasing intensity.

At maximum tension, the sound should feel huge, powerful, expansive and controlled.

# 11. ACCEPTANCE CRITERIA

The project is complete only when the VST3 compiles, the standalone application launches, audio processing works, noise generation works without input, all DSP modules function, the master knob controls the complete effect, the GUI works, automation is smooth, states restore correctly and DSP tests pass.

Do not leave placeholders, unfinished implementations or unnecessary TODOs. Do not claim successful builds or tests unless actually executed.

# 12. IMPLEMENTATION INSTRUCTIONS

Start by inspecting the environment and creating a concise implementation plan. Then write the actual project files directly. Work incrementally, compile frequently and resolve errors autonomously. Make sensible engineering decisions without asking unnecessary questions. Prioritize audio quality, real-time safety, clean architecture and usability.

Reference documentation:
- https://github.com/juce-framework/JUCE/tree/master/examples/CMake/AudioPlugin
- https://docs.juce.com/develop/classjuce_1_1AudioProcessorValueTreeState.html
- https://juce.com/tutorials/tutorial_dsp_introduction/
- https://juce.com/tutorials/tutorial_audio_processor_graph/

---

## Factory Amendments

- **2026-10-29:** Built inside the VST FACTORY monorepo at `plugins/TENSION/` (not a standalone `Tension/` repo); CMake, build, test and install handled by the factory's build-automation pipeline.
- **2026-10-29:** AU (Component) target added alongside VST3 + Standalone — the factory install script ships all three formats on macOS. Windows compatibility is preserved at the source level per §2.
- **2026-10-29:** Origin note — TENSION descends from a DROP ideation session ("remake DROP as a single knob"). DROP 1.0.0 stays installed and untouched; proven DROP code (dB-linear noise riser curves, reverb/delay send architecture, output safety scrubbing) may be ported where it fits this spec.
