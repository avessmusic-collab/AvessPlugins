# KickDesigner2 - Implementation Plan

**Date:** 2026-08-28
**Complexity Score:** 5.0 (Complex — capped)
**Strategy:** Phase-based implementation

complexity_score: 5.0

---

## Complexity Factors

- **Parameters:** **59 APVTS** (51 Float, 3 Choice, 5 Bool) → `min(59/5, 2.0)` = **2.0** (capped)
  - v1 core 45 + **v2 SAMPLE group 14** (`parameter-spec.md` v2, 2026-08-28). `bodyAttack` intentionally not exposed.
- **Algorithms:** **~17** DSP components
  - KickEngine, KickVoice, **SamplePlayer**, BodyOscillator, PitchEnvelope, AmplitudeEnvelope, SubOscillator, ClickGenerator, NoiseGenerator, TailGenerator, TransientShaper, Waveshaper/Saturator (7-curve morph), OutputStage (tone + stereo + limiter + mix + DC), OversamplingProcessor, Analyzer (waveform + FFT spectrum) + **SampleLibrary** (message-thread folder/decode)
- **Features:** **6**
  - FFT / frequency-domain spectrum analyzer (+1)
  - 3-band tone + Linkwitz-Riley mono-crossover band split (+1)
  - Envelope / modulation system (pitch env, per-layer AD envelopes, transient follower) (+1)
  - External MIDI trigger + velocity routing (+1)
  - **Sample playback: interpolated resampling + trim/reverse + AD + filter/crush (+1)**
  - **Managed sample library: folder scan/watch, drag-drop import, decode, atomic buffer hand-off (+1)**
- **Raw total:** 2.0 + 17 + 6 = **25.0**
- **Final (capped at 5.0):** **5.0**

Additional scope multipliers (not scored but drive the phase count): `juce::dsp::Oversampling` wrapping the **entire voice + master chain** (AD-10) with glitch-free runtime factor switching + `fsOversampled` coefficient refresh; **sample-playback engine + managed file library + drag-drop (AD-11/AD-12)**; native JUCE UI (custom LookAndFeel + 7 custom components, resizable); preset manager (factory BinaryData + user disk I/O); 17 factory presets; automated test suite + offline-render WAV utility.

---

## Stages

- Stage 0: Research ✓ (`architecture.md`)
- Stage 0: Planning ✓ (this file)
- Stage 1: Foundation + Shell (CMake, APVTS **59 params**, IDs/layout, bus config, `currentSampleName` state, native editor stub) — **built at v1 (45); amended to v2 (59) — see Stage 1 v2 Amendment below**
- Stage 2: DSP — **12 phases** (2.1 … 2.11 + **2.6b Sample player + library**)
- Stage 3: GUI — 4 phases (3.1 … 3.4) incl. `SampleBrowser` + drag-drop
- Stage 3: Validation (17 presets, pluginval, offline-render tests, changelog)
- Repo Stage 17: Profile & optimize — **lock `oversampling` default (2× vs 4×)**
- Repo Stage 18: Release build VST3 / AU / Standalone; archive `MinimalKick` once ✅ Working

Compile + run the relevant tests + clear all errors and warnings after every phase before continuing (creative-brief development-order rule).

---

## Complex Implementation (Score ≥ 3.0)

Phase breakdown follows the creative brief's 18-step development order, grouped into the repo's Stage 1 / Stage 2 / Stage 3 structure with a compile + test checkpoint after each phase.

### Stage 1: Foundation + Shell

**Goal:** Buildable, loadable instrument with the full 45-parameter APVTS and an empty (silent) engine, plus a native editor stub.

**Components / work:**
- `plugins/KickDesigner2/CMakeLists.txt` — `juce_add_plugin(... IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE FORMATS VST3 AU Standalone COMPANY_NAME "PluginFreedom" PLUGIN_MANUFACTURER_CODE Plgf PLUGIN_CODE Kd02 ...)`; link `juce_audio_utils juce_dsp juce_gui_basics`; `juce_generate_juce_header` **after** `target_link_libraries` (Pattern #1); `juce_add_binary_data` target for factory presets (placeholder now).
- `Source/PluginProcessor.{h,cpp}` — output-only stereo `BusesProperties` in the **constructor** (Pattern #4 / #22); `AudioProcessorValueTreeState` member + `UndoManager`; `getStateInformation`/`setStateInformation` with `stateVersion = 1`.
- `Source/Parameters/ParameterIDs.h` — one `constexpr` string ID per parameter (single source of truth).
- `Source/Parameters/ParameterLayout.h` — table-driven `createParameterLayout()` producing all 45 params with exact ranges/defaults/skews from `parameter-spec.md` v1.
- `Source/PluginEditor.{h,cpp}` — minimal native `AudioProcessorEditor` (generic sliders or a plain placeholder), resizable with `setResizeLimits` + fixed aspect constrainer.
- `Source/Utilities/DSPUtils.{h,cpp}` — skeleton (denormal flush, sanitize, dbToGain, expDecayCoef, pitchRatio, DCBlocker, equalPowerGains).
- `Source/Tests/OfflineRender.{h,cpp}` — headless processor host: feed a note-on, render N seconds, write WAV (`juce::WavAudioFormat`). Used by later phases.

**Checkpoint:**
- [ ] Builds clean (0 warnings) for VST3 + AU + Standalone.
- [ ] Loads in Ableton / Logic / Reaper as an **instrument**; receives MIDI.
- [ ] All parameters visible in the host generic editor with correct names/ranges/defaults.
- [ ] `getStateInformation` → `setStateInformation` round-trips exactly (unit test).
- [ ] Produces silence (no engine yet), no crashes on note-on/off.

#### Stage 1 v2 Amendment — SAMPLE parameters (do before Phase 2.1)

Stage 1 was built and validated at **v1 (45 params)**. `parameter-spec.md` v2 adds the 14-parameter SAMPLE group. Amend the shell:
- `ParameterIDs.h` — add 14 IDs: `synthEnable`, `sampleEnable`, `sampleLevel`, `sampleStart`, `sampleEnd`, `sampleReverse`, `sampleTune`, `sampleFine`, `sampleMidiTrack`, `sampleAttack`, `sampleDecay`, `sampleHP`, `sampleLP`, `sampleCrush`.
- `ParameterLayout.h` — add 10 floats to the table (`FloatParamSpec` array 41 → 51) + 4 `AudioParameterBool` (defaults: `synthEnable` true, `sampleEnable` false, `sampleReverse` false, `sampleMidiTrack` true).
- `PluginProcessor` — bump `stateVersion` to **2**; add the `currentSampleName` `ValueTree` string property to `getStateInformation`/`setStateInformation` round-trip (resolution/decoding is Phase 2.7b — for now just persist the string). Confirm a `stateVersion == 1` blob still loads.
- **Checkpoint:** clean rebuild (0 warnings) VST3+AU+Standalone · pluginval `--strictness-level 10` SUCCESS · zero-drift check now covers 59 IDs · auval shows 59 parameters · v1 state blob loads without error.

---

### Stage 2: DSP Phases

#### Phase 2.1: OS-region shell + MIDI-triggered basic kick (body + amp env)
**Goal:** Establish the **oversampling region as the substrate** (AD-10) — `processSamplesUp`(zero block) → whole-voice render at `fsOversampled` → `processSamplesDown` → base-rate DC blocker + analyzer tap. Then: note-on → sine body with fixed-fast attack + exponential decay, monophonic, rendered inside that region.
**Components:** `OversamplingProcessor` scaffold (4 `dsp::Oversampling<float>` pre-built, **`oversampling` pinned to `1x` for now** so `fsOversampled == fs` and DSP is trivially verifiable), `KickEngine` OS orchestration + `fsOversampled` coefficient plumbing, `KickVoice` (single), `BodyOscillator` (phase accumulator @ `fsOversampled`), `AmplitudeEnvelope` (2 ms atk, exp decay), `KickEngine` MIDI dispatch (sample-accurate sub-block split), pitch resolver (`tuneMode`, `tune`, `fineTune`, `fundamental`, note #).
**Params live:** `fundamental`, `bodyLevel`, `bodyDecay`, `tuneMode`, `tune`, `fineTune`, `velSensitivity` (level only). (`oversampling` present but forced to `1x` until Phase 2.10.)
**Checkpoint:**
- [ ] `processSamplesUp`/`Down` pair in place; at `1x` the path is bit-transparent aside from intended DSP.
- [ ] MIDI note → audible sine kick; pitch correct in both tune modes (AD-6 offset behaviour).
- [ ] Fixed 2 ms attack is click-free; decay matches `bodyDecay` (offline WAV envelope check within ±5 %).
- [ ] Velocity scales level per `velSensitivity`.
- [ ] No NaN/Inf; silence after voice completes. No audio-thread allocation (assertion build).

#### Phase 2.2: Pitch envelope
**Goal:** Punchy pitch drop in ratio/log domain, phase-continuous.
**Components:** `PitchEnvelope` (normalised exp contour, `k` from `pitchCurve`), BodyOscillator per-sample frequency + phase integration.
**Params live:** `pitchStart`, `pitchTime`, `pitchCurve`.
**Checkpoint:**
- [ ] Offline WAV shows exponential frequency fall from `fundamental×pitchStart` → `fundamental`, reaching fundamental at `pitchTime`.
- [ ] `pitchCurve` 0 = audible sweep, 1 = snap+settle (spectrogram check).
- [ ] Zero phase discontinuity / clicks at trigger and through the fall.
- [ ] Default patch (55 Hz, 4×, ~50 ms, curve 0.7) reads as punch, not laser.

#### Phase 2.3: Amp-env refinement + transient shaper + click-free retrigger
**Goal:** Voice lifecycle robustness + `TransientShaper` + 2-voice crossfade retrigger.
**Components:** `KickVoice voices[2]` + `active` toggle, 3 ms equal-power crossfade, `TransientShaper` (dual envelope follower, ±6 dB).
**Params live:** `transientAttack`, `transientSustain`.
**Checkpoint:**
- [ ] Rapid retrigger (e.g. 1/32 notes at 174 BPM) is click-free — offline sample-slew test around each trigger passes.
- [ ] `transientAttack` > 0 sharpens, < 0 softens the attack; `transientSustain` shifts body level.
- [ ] Old voice frees after crossfade (no voice leak, CPU stable under machine-gun triggering).

#### Phase 2.4: Click generator
**Goal:** 3-part synthesised click (no samples), band-limited.
**Components:** `ClickGenerator` — filtered noise burst (0.50) + transient osc w/ pitch drop (0.35) + windowed impulse (0.15); `StateVariableTPTFilter` BP/LP; raised-cosine window LUT.
**Params live:** `clickLevel`, `clickTone`, `clickTime`, `clickPitch`, `velSensitivity` (click routing).
**Checkpoint:**
- [ ] Click audible and tunable across full `clickTone`/`clickPitch`/`clickTime` ranges.
- [ ] No alias spikes in the spectrum at `clickTime = 0.1 ms` (FFT check; at `1x` still, so this validates band-limiting-by-construction independent of OS).
- [ ] Impulse is windowed (no bare 1-sample spike).

#### Phase 2.5: Sub oscillator
**Goal:** Independent mono sub, phase-stable.
**Components:** `SubOscillator` (fixed `subFreq`, phase-0 on trigger, own exp env), summed L=R.
**Params live:** `subLevel`, `subFreq`, `subDecay`.
**Checkpoint:**
- [ ] Sub layer audible; frequency independent of pitch envelope.
- [ ] Null test: two identical triggers, one inverted, summed → silence (phase determinism).
- [ ] Sub summed identically to L and R (mono).

#### Phase 2.6: Tail generator
**Goal:** Sustained tail / rumble with its own length/tone/drive.
**Components:** `TailGenerator` — dedicated LF sine @ `fundamentalEff`, slow ~10 ms attack, exp decay, `StateVariableTPTFilter` LP, `tanh` drive. All rendered inside the OS region (currently `1x`); `tailDrive` is oversampled for real once Phase 2.10 enables 2×+.
**Params live:** `tailLevel`, `tailLength`, `tailTone`, `tailDrive`.
**Checkpoint:**
- [ ] Short `tailLength` = tight; long = drone/rumble.
- [ ] `tailTone` sweeps dark→bright; tail sits behind the transient (not competing with the click).

#### Phase 2.7: Noise generator
**Goal:** Optional white/pink/filtered noise layer (default off).
**Components:** `NoiseGenerator` — `Random`, Paul Kellet pink filter, `StateVariableTPTFilter` tilt/BP.
**Params live:** `noiseLevel`, `noiseDecay`, `noiseTone`, `noiseType`.
**Checkpoint:**
- [ ] All 3 `noiseType` options produce the expected spectrum.
- [ ] `noiseLevel = 0` (default) → exactly silent (no idle noise).

#### Phase 2.7b: Sample player + library (v2 — AD-11/AD-12)
**Goal:** The 6th layer. `SampleLibrary` (message-thread folder + decode + atomic hand-off) → `SamplePlayer` (in-region resample → trim/reverse → AD → HP/LP → crush) → `synthEnable` / `sampleEnable` gates → layer sum with the synth.
**Components:**
- `Source/Sampling/SampleLibrary.{h,cpp}` — `AudioFormatManager` (WAV/AIFF/FLAC/CAF), managed folder `…/KickDesigner2/Samples/` (create on first run), sorted bank list, `prev/next/list`, lookup-by-name, drag-drop import (validate ≤ 5 s / ≤ 2 ch / readable → copy → rescan), decode to `SampleBuffer { AudioBuffer<float>, sourceRate, rootNote=24 }`.
- `Source/DSP/SamplePlayer.{h,cpp}` — `juce::Interpolators::Lagrange` per channel, `ratio = sourceRate/fsOS · 2^((tune+fine/100+midiOff)/12)`, `[start,end]` clamp + 1 ms fades + reverse, raised-cosine attack + exp decay, `StateVariableTPTFilter` HP→LP (vs `fsOS`), bit `lerp(16,4)` + S&H `lerp(1,16)` crush.
- `PluginProcessor` — `std::atomic<const SampleBuffer*> currentSample` / `pendingSample`; publish + retire-when-unreferenced on the message thread; `currentSampleName` already in state from the Stage 1 v2 amendment.
- `KickVoice` — grab `currentSample` at `noteOn` (hold for voice life), pass `noteNumber`; smoothed 0/1 gates for `synthEnable`/`sampleEnable`.
**Params live:** `synthEnable`, `sampleEnable`, `sampleLevel`, `sampleStart`, `sampleEnd`, `sampleReverse`, `sampleTune`, `sampleFine`, `sampleMidiTrack`, `sampleAttack`, `sampleDecay`, `sampleHP`, `sampleLP`, `sampleCrush`.
**Test WAVs:** render 3–4 kicks with `OfflineRender` (synth engine) to use as sample-bank fixtures.
**Checkpoint:**
- [ ] Load a fixture WAV → plays on note-on; `sampleEnable` off (default) → silent; `synthEnable` off → pure sample through the chain.
- [ ] Resample accuracy: a rendered 100 Hz sine at `sampleTune = +12` reads 200 Hz ±1 %, level within ±0.5 dB; `−12` reads 50 Hz with no added aliasing above −60 dBFS.
- [ ] `sampleStart`/`sampleEnd` trim to the expected window (sample-count check); `sampleReverse` produces the time-reversed buffer; both click-free (1 ms fades).
- [ ] `sampleAttack`/`sampleDecay` reshape the envelope (offline WAV env check); `sampleHP`/`sampleLP` at extremes are transparent; `sampleCrush = 1` degrades bit-depth + rate audibly with no NaN.
- [ ] Missing-file recall: preset referencing an absent sample → sample layer silent, notice shown, name retained, synth unaffected.
- [ ] Hot-swap under load: machine-gun trigger while switching the bank → no glitch, no crash, no leak (buffers retired).
- [ ] **Zero audio-thread allocation** with the sample layer active (assertion build / profiler).
- [ ] Sample + synth both on → layer sum is their sum (null test against separate renders).

#### Phase 2.8: Distortion morph
**Goal:** Continuous 7-curve morph, loudness-compensated. **Isolated test before wiring `driveMix`.**
**Components:** `Waveshaper/Saturator` — locked order `tanh→cubic→asym→softclip→hardclip→foldback→bitcrush`; 6-segment equal-gain crossfade; adaptive short-window RMS makeup seeded by static prime table; parallel clean/shaped blend. Runs inside the OS region like everything else (AD-10); currently `1x`, real oversampling benefit lands at Phase 2.10.
**Params live:** `drive`, `character`, `driveMix`.
**Checkpoint:**
- [ ] Sweeping `character` 0→1 at fixed `drive` keeps integrated RMS within ±1.5 dB (offline sweep test).
- [ ] Each curve individually matches its transfer function (unit test on sample values).
- [ ] `driveMix` = 0 → unity clean; foldback iteration bounded; DC handled (asym/foldback/bitcrush).
- [ ] No NaN/Inf at `drive = 1`, `character = 1`.

#### Phase 2.9: Tone / output / stereo
**Goal:** Pre-distortion 3-band tone, stereo width with mono lows, output gain, safety limiter, mix, DC blocker.
**Components:** `OutputStage` — 3× `dsp::IIR` (130/750/5000 Hz, invoked pre-distortion, in-region), `dsp::LinkwitzRileyFilter` split @ 130 Hz + M/S width (in-region), `dsp::Gain` (in-region), soft-clip `tanh` limiter −0.5 dBFS **in-region** (it's a nonlinearity — AD-10), equal-power mix (processed↔silence, in-region), then `processSamplesDown`, then the base-rate 5 Hz DC blocker. Coefficients computed against `fsOversampled`.
**Params live:** `low`, `mid`, `high`, `bodyWidth`, `clickWidth`, `outputWidth`, `output`, `limiter`, `mix`.
**Checkpoint:**
- [ ] Tone bands measure correct shelf/bell response; sit pre-distortion (heavy drive stays "tight").
- [ ] Content < 130 Hz is forced mono (correlation = 1.0); `outputWidth` 0/0.5/1 = mono/unity/wide above 130 Hz.
- [ ] `limiter = on` never exceeds −0.5 dBFS; `limiter = off` can exceed 0 dBFS.
- [ ] Final DC offset < −60 dBFS mean; sub at 25 Hz not thinned.

#### Phase 2.10: Enable real oversampling factors + glitch-free switching
**Goal:** Unpin `oversampling` (the region shell already exists from Phase 2.1). Enable 2×/4×/8×, `fsOversampled` coefficient refresh on factor change, glitch-free switching, correct latency reporting.
**Components:** `OversamplingProcessor` — activate the 4 pre-built `dsp::Oversampling<float>` (1×/2×/4×/8×, `filterHalfBandPolyphaseIIR`, `useIntegerLatency = true`); atomic pointer swap + ~64-sample fade; audio-thread recompute of all in-region coefficients for the new `fsOversampled` during the fade; `setLatencySamples` on message thread + in `prepareToPlay`.
**Params live:** `oversampling` (default index 1 = `2x`) — now functional.
**Checkpoint:**
- [ ] Reported latency matches `round(activeOs->getLatencyInSamples())` per factor; 1× → 0.
- [ ] Switching factors during playback is click-free (fade + coefficient refresh cover the swap) in Ableton / Logic / Reaper.
- [ ] Every in-region stage sounds pitch/time-correct at all 4 factors (coefficient refresh verified — no detune/decay drift on switch).
- [ ] Aliasing at `drive = 1` (and `bodyHarmonics = 1`, `character = 1`) visibly reduced 1×→2×→4×→8× (FFT).
- [ ] No audio-thread allocation (profiler / assertion build).

#### Phase 2.11: Parameter smoothing, macros, state
**Goal:** All 59 params smoothed; 4 macros as effective-value offsets; state version + preset-name + `currentSampleName` property.
**Components:** `juce::SmoothedValue` per continuous param (Multiplicative for frequencies); `KickEngine` macro layer (0.5 neutral, bipolar smoothed offsets per AD-mapping); `stateVersion`, `currentPresetName/Path` on the APVTS tree.
**Params live:** `macroPunch`, `macroBody`, `macroCrush`, `macroTail` + smoothing for all.
**Checkpoint:**
- [ ] No zipper noise on fast automation of any parameter.
- [ ] Each macro produces a musical change at the extremes, neutral at 0.5; underlying params remain independently automatable.
- [ ] Full-mix automation pass in a host (all params) — no clicks, no runaway, no NaN.
- [ ] State round-trip still exact with `stateVersion`.

---

### Stage 3: GUI Phases

#### Phase 3.1: LookAndFeel + layout + parameter binding
**Goal:** Premium dark native UI (per the approved v2 mockup — soft slate-grey chassis, kick oscilloscope, PUNCH/BODY/CRUSH/TAIL macro modules, engine strip + SAMPLE module); every control bound to the correct parameter ID.
**Components:** `Source/UI/LookAndFeel`, `Source/UI/Knob` (drag + wheel + double-click reset + readout + tooltip + `beginChangeGesture`/`endChangeGesture` bracketing), section panels (Pitch/Body/Click/Tail/Sub/Noise/Drive/Tone/Stereo/Output), **Sample module** (enable toggles + tweak knobs), macro modules (Punch/Body/Crush/Tail), global strip (Oversampling/Limiter/Output/Mix), header (wordmark + preset prev/next + Save/Undo/Redo/Randomize/Mutate). `juce::SliderAttachment` / `ButtonAttachment` / `ComboBoxAttachment` for all **59**. `FlexBox`/`Grid` layout, resizable with fixed aspect + min size.
**Checkpoint:**
- [ ] Every one of the 59 controls binds to the correct APVTS ID (audit against the Parameter Mapping table).
- [ ] Double-click resets to default (incl. `output` → normalised ≈ 0.6667).
- [ ] Host automation moves the UI; UI moves host parameters; gestures bracket correctly (Ableton touch/latch records).
- [ ] Layout holds at min / mid / max window size.

#### Phase 3.2: Waveform + spectrum analyzers
**Goal:** Real-time displays with zero audio-thread cost.
**Components:** `Source/UI/WaveformDisplay` (double-buffered one-kick-per-trigger capture via atomic index), `Source/UI/SpectrumDisplay` (`AbstractFifo` drain + `dsp::FFT(11)` + Hann on a 30 Hz `Timer`), optional `EnvelopeDisplay`.
**Checkpoint:**
- [ ] Waveform captures exactly one full kick per trigger; updates within one Timer tick.
- [ ] Spectrum updates ~30 fps; FFT runs on the message thread only.
- [ ] Zero allocation / locks / FFT in `processBlock` (assertion build + profiler).
- [ ] Repaint does not starve the message thread (CPU check with UI open).

#### Phase 3.3: Preset system + sample browser + Randomize / Mutate / Undo
**Goal:** Factory + user presets; sample bank UI; constrained randomisation; undo/redo.
**Components:** `Source/Presets/PresetManager` (factory via `BinaryData`, user disk under `…/KickDesigner2/`), `Source/UI/PresetBrowser` (prev/next + list + Save), **`Source/UI/SampleBrowser`** (prev/next/list over `SampleLibrary`, current-file name, drag-drop target with visible empty-bank state + missing-file notice; the whole editor is also a `FileDragAndDropTarget`), Randomize/Mutate (genre-aware bounds + correlations, exclusion list **incl. all `sample*` / enables / selection**), `UndoManager` transactions.
**Factory set (17):** Clean · House · Techno · Hard Techno · Hardstyle · Hardcore · Industrial · Sub Heavy · Short · Long · Distorted · Clicky · Punchy · Warehouse · EDM · Trap · Cinematic — **all synth-only (`sampleEnable` off)**. Default patch loads on first instantiation.
**Checkpoint:**
- [ ] All 17 factory presets load and round-trip; each is a recognisable, usable kick for its genre.
- [ ] User Save → file on disk (+ `currentSampleName`) → reload after re-instantiation restores exactly, sample included when the file is present.
- [ ] Drag an audio file onto the editor → copied to `Samples/`, appears in the browser, selected, `sampleEnable` on, plays.
- [ ] Empty bank shows a clear "drop a kick here" state; missing-sample recall shows a non-modal notice, synth unaffected.
- [ ] Randomize: large majority usable; never touches `oversampling`/`limiter`/`output`/`mix`/tuning/`velSensitivity`/macros/**`synth*`·`sample*`·selection**.
- [ ] Mutate preserves character; Undo/Redo reverts Randomize/Mutate/preset-load (not sample selection).
- [ ] Missing user preset on restore → falls back to Default with a non-modal notice.

#### Phase 3.4: Final UI polish
**Goal:** Ship-quality visuals + interaction.
**Components:** typography/spacing/depth pass, tooltips + precise value readouts + unit formatting for all controls, centre-detent visuals for bipolar knobs, style labels on `character` (Tanh/Cubic/Fold…), analyzer overlay toggle, keyboard/accessibility basics.
**Checkpoint:**
- [ ] Matches the "premium dark modern" brief; no skeuomorphism.
- [ ] Every knob shows sensible units + precision + tooltip.
- [ ] No visual glitches on resize / preset change / automation.

---

### Stage 3: Validation
- [ ] 17 factory presets committed (generated or `.xml` via `BinaryData`), all synth-only.
- [ ] `pluginval` (strictness 10) passes for VST3 + AU.
- [ ] Automated test suite green: parameter ranges (59) · state round-trip (incl. `currentSampleName`, `stateVersion 1→2`) · preset loading · MIDI trigger · voice lifecycle · pitch-env shape · amp-env shape · oversampling latency + switching · rapid-retrigger click detection · NaN/Inf scan · DC-offset bound · output stability · silence-after-completion · deterministic offline render · **sample: resample accuracy · trim/reverse · missing-file · hot-swap-under-load · zero audio-thread alloc · sample+synth sum**.
- [ ] `OfflineRender` utility renders a kick to WAV for manual inspection; also renders the sample-bank test fixtures.
- [ ] Sample library: drag-drop copy + rescan + select works; empty-bank + missing-file UI states verified.
- [ ] Changelog / NOTES.md updated.
- [ ] Zero compiler warnings across all targets.

### Repo Stage 17 — Profile & optimize
- [ ] CPU profile at 44.1 / 48 / 96 kHz, 2× vs 4× oversampling, UI open/closed, **synth-only vs sample-only vs blended**.
- [ ] **Lock the `oversampling` default** (keep `2x` or promote to `4x`) — update `parameter-spec.md` note + `ParameterLayout.h` default index.
- [ ] Optimise hot paths if needed (per-block pitch-env interpolation, fast `tanh`, sample interpolation order).

### Repo Stage 18 — Release
- [ ] VST3 / AU / Standalone release build, signed, installed, cache-cleared (Pattern #18).
- [ ] On ✅ Working: set `MinimalKick` → 🗑️ Archived in `PLUGINS.md`.

---

## Implementation Flow

- Stage 1: Foundation + Shell (+ **v2 Amendment**: add 14 SAMPLE params, `stateVersion` → 2, `currentSampleName`)
- Stage 2: DSP — **12 phases** (2.1 → 2.11 + **2.7b Sample player + library**), compile + test + zero-warnings after each
- Stage 3: GUI — 4 phases (3.1 → 3.4) incl. `SampleBrowser` + drag-drop, compile + test after each
- Stage 3: Validation — presets, pluginval, test suite, offline render, changelog
- Repo Stage 17: Profile → lock oversampling default
- Repo Stage 18: Release → archive MinimalKick

---

## Implementation Notes

### Critical Files
- `Source/DSP/KickEngine.{h,cpp}` — orchestration, macro layer, MIDI dispatch, retrigger crossfade, analyzer taps, latency.
- `Source/DSP/PitchEnvelope.{h,cpp}` — ratio-domain contour math (AD / research §1); highest "get the feel right" risk.
- `Source/DSP/Waveshaper.{h,cpp}` — 7 transfer functions + crossfade + adaptive RMS makeup (AD-5); loudness-continuity risk.
- `Source/DSP/OversamplingProcessor.{h,cpp}` — 4 pre-built instances wrapping the **whole voice + master chain** (AD-10), glitch-free swap + `fsOversampled` coefficient refresh, latency reporting (AD-2); switching risk. Built as the substrate in Phase 2.1.
- `Source/DSP/OutputStage.{h,cpp}` — tone (pre) + LR mono crossover + M/S + soft-clip limiter + mix + DC.
- `Source/DSP/SamplePlayer.{h,cpp}` — in-region interpolated resample + trim/reverse + AD + HP/LP + crush (AD-11); reads a `const SampleBuffer*` it does not own.
- `Source/Sampling/SampleLibrary.{h,cpp}` — managed folder scan/watch, drag-drop import + validate, `AudioFormatManager` decode, atomic buffer publish + retire (AD-11/12); message thread only; the other high-risk component.
- `Source/Parameters/ParameterLayout.h` — table-driven **59-param** APVTS; single point to get ranges/defaults/skews right.
- `Source/Parameters/ParameterIDs.h` — ID single source of truth.
- `Source/Presets/PresetManager.{h,cpp}` — factory (BinaryData) + user disk I/O (message thread only); persists `currentSampleName`.
- `Source/UI/Knob.{h,cpp}` + `LookAndFeel` — reused by every panel; gesture bracketing.
- `Source/UI/WaveformDisplay.{h,cpp}` / `SpectrumDisplay.{h,cpp}` — lock-free capture + message-thread FFT.
- `Source/UI/SampleBrowser.{h,cpp}` — bank list + drag-drop target + empty/missing states.
- `Source/Tests/OfflineRender.{h,cpp}` + `DSPTests.cpp` + `KickEngineTests.cpp` — the WAV-render test utility and assertions (also renders the sample-bank test fixtures).

### Thread Safety
- Parameter reads: `apvts.getRawParameterValue(id)->load()` once per block → macro resolve → `SmoothedValue`.
- No shared state between the two `KickVoice` instances; voice state is audio-thread-local.
- Analyzer: audio thread does bounded `memcpy` + atomic stores only; FFT + paint on the message-thread `Timer`.
- OS switch: `std::atomic<Oversampling*>` + `setLatencySamples` from the message-thread listener / `prepareToPlay` only.
- **Sample buffer:** `std::atomic<const SampleBuffer*>` `pending`/`current`; audio thread swaps `pending→current` at block start and captures `current` per `noteOn` (held for the voice); previous buffers retired on the message thread once no voice references them. No refcount on the audio thread.
- Preset + file I/O + sample decode/copy/scan: message thread only.
- No mutexes in the signal path.

### Performance
- **Whole voice renders in-region (AD-10)** so cost scales ~linearly with OS factor. Rough: ~10 % (1×) · ~30–40 % (2×, default) · ~60–75 % (4×) · ~90 %+ (8×) of one core (monophonic, 48 kHz). Label `8×` "high CPU".
- **AD-10 escape hatch** (Stage 17): if 4×/8× too heavy, render the pure-sine layers (sub / body pre-`bodyHarmonics` / tail osc / pre-filtered noise) at base rate and up-sample into the OS buffer; keep only nonlinear stages + tone/transient in-region.
- Hot paths: OS half-band filters; per-sample `exp`/`pow` in the pitch envelope (mitigate with per-block linear interpolation of the normalised envelope); `tanh` in `bodyHarmonics`/shaper/limiter (share one fast approximation if profiling demands).
- Analyzer: FFT size 2048 (order 11); 30 fps repaint; drop to 1024 / 20 fps as a fallback.

### Latency
- Only source is `dsp::Oversampling`. Limiter = 0 (zero-latency soft-clip). Tone/stereo/mix = 0.
- `setLatencySamples((int) std::round(activeOs->getLatencyInSamples()) + 0)`; updated on every `oversampling` change (message thread) and in `prepareToPlay`.
- Host PDC re-scan on change expected; ~64-sample output fade hides the discontinuity.

### Denormal / NaN / DC Protection
- `juce::ScopedNoDenormals` at the top of `processBlock`.
- `DSPUtils::flushDenormal` on envelope + filter states; `DSPUtils::sanitize` after the shaper and after the limiter (`jassertfalse` in debug).
- 5 Hz one-pole DC blocker on the final bus **at base rate, after `processSamplesDown`** (corner below 25 Hz sub); lighter blocker on the body path after `bodyHarmonics` (in-region).
- Foldback `while(|u|>1)` guarded with a max-iteration break.

### Known Challenges / References
- **Pitch envelope "punch not laser":** ratio/log domain only, phase-continuous. Migrate MinimalKick's `f = f0·2^(env·semis/12)` formula (not its code). Two-stage snap+settle available as a Stage-2 tuning option.
- **Distortion loudness continuity:** adaptive RMS makeup seeded by a static prime table; fallback to a 2-D `curve×drive` static LUT if pumping appears on short kicks.
- **Oversampling scope (AD-10):** whole voice + master chain (through the `tanh` limiter) is inside one OS region — not just the master waveshaper — so every nonlinearity is oversampled by construction, not by a "frequency is low" argument. Region shell built in Phase 2.1 (pinned 1×), factors enabled in Phase 2.10.
- **Oversampling switching:** pre-build per factor, atomic swap + ~64-sample fade + audio-thread `fsOversampled` coefficient refresh, `useIntegerLatency = true`. Fallback: freeze `oversampling` as a prepare-time-only choice.
- **Click aliasing:** generated in-region AND band-limited by construction (windowed impulse ≥8 samples at `fsOversampled`, pre-filtered noise, band-limited osc). Fallback: drop the impulse component / widen the window.
- **Mono phase stability:** phase-reset all oscillators on trigger; null test. Fallback: DC-servo instead of HP if the sub thins.
- **Native UI (no repo precedent):** build LookAndFeel + Knob first; FlexBox layout; fallback to fixed-size UI for v1 if resizable is fiddly.
- **Parameter count:** **59 APVTS** (51 Float / 3 Choice / 5 Bool) — v1 core 45 + v2 SAMPLE 14 (`parameter-spec.md` v2). `bodyAttack` not exposed. Do not invent a parameter.
- **Locked-spec vs brief distortion order:** parameter-spec order (`tanh→cubic→asym→softclip→hardclip→foldback→bitcrush`) is authoritative.
- **Sample-buffer threading (AD-11):** in-RAM buffer, `std::atomic<const SampleBuffer*>`, voice captures the pointer at `noteOn` and holds it, previous buffer retired on the message thread once unreferenced. No filesystem / allocation / lock on the audio thread. Fallbacks: WAV/AIFF only; rescan-on-browse (no watcher); `Lagrange` interpolation if `WindowedSinc` too costly at 8×.
- **No factory samples (AD-12):** bank empty until the user fills it; every factory preset synth-only. UI must make the drop target + empty state obvious.

---

## Open Questions — RESOLVED in Stage 0 (see `architecture.md` Architecture Decisions)

| # | Question | Resolution |
|---|---|---|
| 1 | Default oversampling 2× vs 4× | **`2x`** provisional (choice index 1); confirmed at repo Stage 17 profiling. **Scope (AD-10):** the OS region wraps the entire voice + master chain (through the `tanh` safety limiter), not just the master waveshaper — every nonlinearity is oversampled by construction. Region built as the substrate in Phase 2.1 (pinned 1×), real factors enabled in Phase 2.10. CPU scales ~linearly with factor; `8×` is a "high CPU" option; base-rate escape hatch for the pure-sine layers documented for Stage 17. |
| 2 | Safety limiter design | **Zero-latency soft-clip ceiling −0.5 dBFS** as `limiter = on`. Short-lookahead true limiter **NOT in v1** (not in locked param list); internal hook `kLimiterLookaheadSamples = 0` for a future spec bump. |
| 3 | Distortion morph ordering + makeup | Locked order `tanh→cubic→asymmetric→soft-clip→hard-clip→foldback→bitcrush` (parameter-spec authoritative); equal-gain linear crossfade; **adaptive short-window RMS makeup** seeded by a static prime table (static 2-D LUT = fallback). Transfer functions specified in `architecture.md`. |
| 4 | Tone position | **Fixed pre-distortion** (not switchable — no parameter). |
| 5 | Pitch envelope contour | **Single `pitchCurve`-driven normalised-exponential snap+settle**, `k = 0.6 + pitchCurve·8.4`; `f(t) = fundamentalEff·pitchStartEff^{e(τ)}`; phase-integrated. Two-stage = optional Stage-2 refinement. |
| 6 | Mono crossover frequency | **Fixed 130 Hz** Linkwitz-Riley split (cannot expose — not in param list). |
| 7 | Click generator blend | **0.50 filtered-noise burst + 0.35 transient osc (with pitch drop) + 0.15 windowed raised-cosine impulse (≥8 samples at `fsOversampled`)**; generated **in-region** (AD-10) AND band-limited by construction. |
| 8 | `fundamental` in MIDI Pitch mode | **Global semitone offset** `12·log2(fundamental/55)` on top of the note (0 at the 55 Hz default); absolute in Fixed Frequency mode. Keeps the knob and `macroBody` meaningful. |
| 9 | `bodyAttack` | **Kept fixed-fast internal** (`kBodyAttackMs = 2.0`, raised-cosine). Not exposed (not in locked list). Soft-attack use case covered by negative `transientAttack`. Accepted deviation from the original spec's 0–20 ms amp attack. |
| 10 | `mix` | **Kept automatable** (locked contract) as an **equal-power blend processed↔silence** (no dry path for an output-only instrument); functions as an automatable fade/mute. Tooltip: "leave at 100 %". |

### v2 SAMPLE engine — decisions + open items (see `architecture.md` AD-11 / AD-12, `parameter-spec.md` v2 Addendum)

| Item | Decision / status |
|---|---|
| Engine model | **Blendable 6th layer** (`SamplePlayer`), not a mode. `synthEnable` on / `sampleEnable` off by default → v1 behaviour preserved. Turn `synthEnable` off for a pure-sample kick. |
| Bank | Plugin-managed folder `…/KickDesigner2/Samples/`; drag-drop import (copy in); presets reference by bare file name. **No factory samples.** |
| Buffer / threading | In-RAM `SampleBuffer`, atomic pointer, voice holds it for its life, retire-when-unreferenced. 5 s length cap. |
| Downstream | Sample sums with synth **before** TransientShaper → identical chain (drive, tone, stereo, limiter, macros, oversampling). `sampleHP/LP/Crush` are sample-only, pre-sum, in-region. |
| **Open — Stage 2** | (a) interpolation order: `Lagrange` vs `WindowedSinc`; (b) `sampleMidiTrack` root: fixed C1 in v1 vs a per-sample root; (c) `sampleDecay` max semantics: exp fade vs "play to trim end"; (d) whether to ship 3–4 synthesised starter WAVs as an optional pack (not v1). |

---

## References

- Creative brief: `plugins/KickDesigner2/.ideas/creative-brief.md`
- Parameter spec (🔒 locked v1): `plugins/KickDesigner2/.ideas/parameter-spec.md`
- DSP architecture: `plugins/KickDesigner2/.ideas/architecture.md`
- Research notes (advisory): `plugins/KickDesigner2/.ideas/research-notes.md`
- UI mockup: none (native JUCE — control types specified inline in `parameter-spec.md`)

**Reference plugins in this repo:**
- `MinimalKick` (🚧 Stage 5) — migrate pitch-env / amp-env / tanh-drive **formulas only**; archive when KD2 is ✅ Working.
- `Drum808` (📦 Installed) — reference for a native-ish drum instrument's MIDI trigger + voice lifecycle patterns.
- `LushPad` (📦 Installed) — reference for `IS_SYNTH` CMake flags + MIDI routing (Pattern #22).
- `AutoClip` (📦 Installed) — reference for a hard-clipper / waveshaper + oversampling arrangement.
- `Transitionist` (📦 Installed) — reference for gesture bracketing + analyzer-style UI updates (patterns transfer even though it is WebView).
