# KickDesigner2 - Implementation Plan

**Date:** 2026-08-28
**Complexity Score:** 5.0 (Complex — capped)
**Strategy:** Phase-based implementation

complexity_score: 5.0

---

## Complexity Factors

- **Parameters:** 45 APVTS parameters (41 Float, 3 Choice, 1 Bool) → `min(45/5, 2.0)` = **2.0** (capped)
  - Note: `parameter-spec.md` header says 42 Float / 46 APVTS / 49 total; the section bodies enumerate 41 Float / 45 APVTS / 48 total. Proceed with 45; reconcile the header with the spec author before Stage 1 sign-off.
- **Algorithms:** ~14 DSP components = **14**
  - KickEngine, KickVoice, BodyOscillator, PitchEnvelope, AmplitudeEnvelope, SubOscillator, ClickGenerator, NoiseGenerator, TailGenerator, TransientShaper, Waveshaper/Saturator (7-curve morph), OutputStage (tone + stereo + limiter + mix + DC), OversamplingProcessor, Analyzer (waveform + FFT spectrum)
- **Features:** **4**
  - FFT / frequency-domain spectrum analyzer (+1)
  - 3-band tone + Linkwitz-Riley mono-crossover band split (+1)
  - Envelope / modulation system (pitch env, per-layer AD envelopes, transient follower) (+1)
  - External MIDI trigger + velocity routing (+1)
- **Raw total:** 2.0 + 14 + 4 = **20.0**
- **Final (capped at 5.0):** **5.0**

Additional scope multipliers (not scored but drive the phase count): `juce::dsp::Oversampling` with glitch-free runtime switching, native JUCE UI (custom LookAndFeel + 6 custom components, resizable), preset manager (factory BinaryData + user disk I/O), 17 factory presets, automated test suite + offline-render WAV utility.

---

## Stages

- Stage 0: Research ✓ (`architecture.md`)
- Stage 0: Planning ✓ (this file)
- Stage 1: Foundation + Shell (CMake, APVTS, IDs/layout, bus config, native editor stub) ← Next
- Stage 2: DSP — 11 phases (2.1 … 2.11)
- Stage 3: GUI — 4 phases (3.1 … 3.4)
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
- [ ] All 45 parameters visible in the host generic editor with correct names/ranges/defaults.
- [ ] `getStateInformation` → `setStateInformation` round-trips exactly (unit test).
- [ ] Produces silence (no engine yet), no crashes on note-on/off.

---

### Stage 2: DSP Phases

#### Phase 2.1: MIDI-triggered basic kick (body + amp env)
**Goal:** Note-on → sine body with fixed-fast attack + exponential decay. Monophonic.
**Components:** `KickVoice` (single), `BodyOscillator` (phase accumulator), `AmplitudeEnvelope` (2 ms atk, exp decay), `KickEngine` MIDI dispatch (sample-accurate sub-block split), pitch resolver (`tuneMode`, `tune`, `fineTune`, `fundamental`, note #).
**Params live:** `fundamental`, `bodyLevel`, `bodyDecay`, `tuneMode`, `tune`, `fineTune`, `velSensitivity` (level only).
**Checkpoint:**
- [ ] MIDI note → audible sine kick; pitch correct in both tune modes (AD-6 offset behaviour).
- [ ] Fixed 2 ms attack is click-free; decay matches `bodyDecay` (offline WAV envelope check within ±5 %).
- [ ] Velocity scales level per `velSensitivity`.
- [ ] No NaN/Inf; silence after voice completes.

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
- [ ] No alias spikes in the spectrum at `clickTime = 0.1 ms` (FFT check, base rate).
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
**Components:** `TailGenerator` — dedicated LF sine @ `fundamentalEff`, slow ~10 ms attack, exp decay, `StateVariableTPTFilter` LP, `tanh` drive (drive deferred into OS section in Phase 2.10).
**Params live:** `tailLevel`, `tailLength`, `tailTone`, (`tailDrive` provisional at base rate until 2.10).
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

#### Phase 2.8: Distortion morph
**Goal:** Continuous 7-curve morph, loudness-compensated. **Isolated test before wiring `driveMix`.**
**Components:** `Waveshaper/Saturator` — locked order `tanh→cubic→asym→softclip→hardclip→foldback→bitcrush`; 6-segment equal-gain crossfade; adaptive short-window RMS makeup seeded by static prime table; parallel clean/shaped blend. (Runs at base rate for now; moved inside OS in Phase 2.10.)
**Params live:** `drive`, `character`, `driveMix`.
**Checkpoint:**
- [ ] Sweeping `character` 0→1 at fixed `drive` keeps integrated RMS within ±1.5 dB (offline sweep test).
- [ ] Each curve individually matches its transfer function (unit test on sample values).
- [ ] `driveMix` = 0 → unity clean; foldback iteration bounded; DC handled (asym/foldback/bitcrush).
- [ ] No NaN/Inf at `drive = 1`, `character = 1`.

#### Phase 2.9: Tone / output / stereo
**Goal:** Pre-distortion 3-band tone, stereo width with mono lows, output gain, safety limiter, mix, DC blocker.
**Components:** `OutputStage` — 3× `dsp::IIR` (130/750/5000 Hz, invoked pre-distortion by the engine), `dsp::LinkwitzRileyFilter` split @ 130 Hz + M/S width, `dsp::Gain`, zero-latency soft-clip limiter (−0.5 dBFS), equal-power mix (processed↔silence), 5 Hz DC blocker.
**Params live:** `low`, `mid`, `high`, `bodyWidth`, `clickWidth`, `outputWidth`, `output`, `limiter`, `mix`.
**Checkpoint:**
- [ ] Tone bands measure correct shelf/bell response; sit pre-distortion (heavy drive stays "tight").
- [ ] Content < 130 Hz is forced mono (correlation = 1.0); `outputWidth` 0/0.5/1 = mono/unity/wide above 130 Hz.
- [ ] `limiter = on` never exceeds −0.5 dBFS; `limiter = off` can exceed 0 dBFS.
- [ ] Final DC offset < −60 dBFS mean; sub at 25 Hz not thinned.

#### Phase 2.10: Oversampling
**Goal:** Wrap the nonlinear section (`tailDrive` + Waveshaper) in `dsp::Oversampling`; glitch-free switching; correct latency.
**Components:** `OversamplingProcessor` — 4 pre-built `dsp::Oversampling<float>` (1×/2×/4×/8×, `filterHalfBandPolyphaseIIR`, `useIntegerLatency = true`), `initProcessing` in `prepareToPlay`; atomic pointer swap + ~64-sample fade; `setLatencySamples` on message thread.
**Params live:** `oversampling` (default index 1 = `2x`).
**Checkpoint:**
- [ ] Reported latency matches `round(getLatencyInSamples())` per factor; 1× → 0.
- [ ] Switching factors during playback is click-free (fade covers the swap) in Ableton / Logic / Reaper.
- [ ] Aliasing at `drive = 1` visibly reduced 2×→4×→8× (FFT).
- [ ] No audio-thread allocation (profiler / assertion build).

#### Phase 2.11: Parameter smoothing, macros, state
**Goal:** All 45 params smoothed; 4 macros as effective-value offsets; state version + preset-name property.
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
**Goal:** Premium dark native UI; every control bound to the correct parameter ID.
**Components:** `Source/UI/LookAndFeel`, `Source/UI/Knob` (drag + wheel + double-click reset + readout + tooltip + `beginChangeGesture`/`endChangeGesture` bracketing), section panels (Pitch/Body/Click/Tail/Sub/Noise/Drive/Tone/Stereo/Output), macro row (Punch/Body/Crush/Tail), global strip (Oversampling/Limiter/Output/Mix), header (wordmark + preset prev/next + Save/Undo/Redo/Randomize/Mutate). `juce::SliderAttachment` / `ButtonAttachment` / `ComboBoxAttachment` for all 45. `FlexBox`/`Grid` layout, resizable with fixed aspect + min size.
**Checkpoint:**
- [ ] Every one of the 45 controls binds to the correct APVTS ID (audit against the Parameter Mapping table).
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

#### Phase 3.3: Preset system + Randomize / Mutate / Undo
**Goal:** Factory + user presets; constrained randomisation; undo/redo.
**Components:** `Source/Presets/PresetManager` (factory via `BinaryData`, user disk under `~/Library/Audio/Presets/PluginFreedom/KickDesigner2/`), `Source/UI/PresetBrowser` (prev/next + list + Save), Randomize (genre-aware per-param bounds + correlations, exclusion list), Mutate (±5–15 % perturbation, same exclusions), `UndoManager` transactions.
**Factory set (17):** Clean · House · Techno · Hard Techno · Hardstyle · Hardcore · Industrial · Sub Heavy · Short · Long · Distorted · Clicky · Punchy · Warehouse · EDM · Trap · Cinematic. Default patch loads on first instantiation.
**Checkpoint:**
- [ ] All 17 factory presets load and round-trip; each is a recognisable, usable kick for its genre.
- [ ] User Save → file on disk → reload after plugin re-instantiation restores exactly.
- [ ] Randomize: large majority of results are usable kicks; never touches `oversampling`/`limiter`/`output`/`mix`/tuning/`velSensitivity`/macros.
- [ ] Mutate preserves character; Undo/Redo reverts Randomize/Mutate/preset-load.
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
- [ ] 17 factory presets committed (generated or `.xml` via `BinaryData`).
- [ ] `pluginval` (strictness 10) passes for VST3 + AU.
- [ ] Automated test suite green: parameter ranges · state round-trip · preset loading · MIDI trigger · voice lifecycle · pitch-env shape · amp-env shape · oversampling latency + switching · rapid-retrigger click detection · NaN/Inf scan · DC-offset bound · output stability · silence-after-completion · deterministic offline render.
- [ ] `OfflineRender` utility renders a kick to WAV for manual inspection.
- [ ] Changelog / NOTES.md updated.
- [ ] Zero compiler warnings across all targets.

### Repo Stage 17 — Profile & optimize
- [ ] CPU profile at 44.1 / 48 / 96 kHz, 2× vs 4× oversampling, UI open/closed.
- [ ] **Lock the `oversampling` default** (keep `2x` or promote to `4x`) — update `parameter-spec.md` note + `ParameterLayout.h` default index.
- [ ] Optimise hot paths if needed (per-block pitch-env interpolation, fast `tanh`).

### Repo Stage 18 — Release
- [ ] VST3 / AU / Standalone release build, signed, installed, cache-cleared (Pattern #18).
- [ ] On ✅ Working: set `MinimalKick` → 🗑️ Archived in `PLUGINS.md`.

---

## Implementation Flow

- Stage 1: Foundation + Shell
- Stage 2: DSP — 11 phases (2.1 → 2.11), compile + test + zero-warnings after each
- Stage 3: GUI — 4 phases (3.1 → 3.4), compile + test after each
- Stage 3: Validation — presets, pluginval, test suite, offline render, changelog
- Repo Stage 17: Profile → lock oversampling default
- Repo Stage 18: Release → archive MinimalKick

---

## Implementation Notes

### Critical Files
- `Source/DSP/KickEngine.{h,cpp}` — orchestration, macro layer, MIDI dispatch, retrigger crossfade, analyzer taps, latency.
- `Source/DSP/PitchEnvelope.{h,cpp}` — ratio-domain contour math (AD / research §1); highest "get the feel right" risk.
- `Source/DSP/Waveshaper.{h,cpp}` — 7 transfer functions + crossfade + adaptive RMS makeup (AD-5); loudness-continuity risk.
- `Source/DSP/OversamplingProcessor.{h,cpp}` — 4 pre-built instances, glitch-free swap, latency reporting (AD-2); switching risk.
- `Source/DSP/OutputStage.{h,cpp}` — tone (pre) + LR mono crossover + M/S + soft-clip limiter + mix + DC.
- `Source/Parameters/ParameterLayout.h` — table-driven 45-param APVTS; single point to get ranges/defaults/skews right.
- `Source/Parameters/ParameterIDs.h` — ID single source of truth.
- `Source/Presets/PresetManager.{h,cpp}` — factory (BinaryData) + user disk I/O (message thread only).
- `Source/UI/Knob.{h,cpp}` + `LookAndFeel` — reused by every panel; gesture bracketing.
- `Source/UI/WaveformDisplay.{h,cpp}` / `SpectrumDisplay.{h,cpp}` — lock-free capture + message-thread FFT.
- `Source/Tests/OfflineRender.{h,cpp}` + `DSPTests.cpp` + `KickEngineTests.cpp` — the WAV-render test utility and assertions.

### Thread Safety
- Parameter reads: `apvts.getRawParameterValue(id)->load()` once per block → macro resolve → `SmoothedValue`.
- No shared state between the two `KickVoice` instances; voice state is audio-thread-local.
- Analyzer: audio thread does bounded `memcpy` + atomic stores only; FFT + paint on the message-thread `Timer`.
- OS switch: `std::atomic<Oversampling*>` + `setLatencySamples` from the message-thread listener / `prepareToPlay` only.
- Preset + file I/O: message thread only.
- No mutexes in the signal path.

### Performance
- Estimate ~25–30 % of one core at 2× OS (monophonic, 48 kHz); ~40–50 % at 4×; ~70 %+ at 8×.
- Hot paths: oversampling half-band filters; per-sample `exp`/`pow` in the pitch envelope (mitigate with per-block linear interpolation of the normalised envelope); `tanh` in the shaper (fast approximation if profiling demands).
- Analyzer: FFT size 2048 (order 11); 30 fps repaint; drop to 1024 / 20 fps as a fallback.

### Latency
- Only source is `dsp::Oversampling`. Limiter = 0 (zero-latency soft-clip). Tone/stereo/mix = 0.
- `setLatencySamples((int) std::round(activeOs->getLatencyInSamples()) + 0)`; updated on every `oversampling` change (message thread) and in `prepareToPlay`.
- Host PDC re-scan on change expected; ~64-sample output fade hides the discontinuity.

### Denormal / NaN / DC Protection
- `juce::ScopedNoDenormals` at the top of `processBlock`.
- `DSPUtils::flushDenormal` on envelope + filter states; `DSPUtils::sanitize` after the shaper and after the limiter (`jassertfalse` in debug).
- 5 Hz one-pole DC blocker on the final bus (corner below 25 Hz sub); lighter blocker on the body path after `bodyHarmonics`.
- Foldback `while(|u|>1)` guarded with a max-iteration break.

### Known Challenges / References
- **Pitch envelope "punch not laser":** ratio/log domain only, phase-continuous. Migrate MinimalKick's `f = f0·2^(env·semis/12)` formula (not its code). Two-stage snap+settle available as a Stage-2 tuning option.
- **Distortion loudness continuity:** adaptive RMS makeup seeded by a static prime table; fallback to a 2-D `curve×drive` static LUT if pumping appears on short kicks.
- **Oversampling switching:** pre-build per factor, atomic swap + fade, `useIntegerLatency = true`. Fallback: freeze `oversampling` as a prepare-time-only choice.
- **Click aliasing:** band-limit by construction (windowed impulse, pre-filtered noise, band-limited osc). Fallback: generate click inside the OS block or drop the impulse component.
- **Mono phase stability:** phase-reset all oscillators on trigger; null test. Fallback: DC-servo instead of HP if the sub thins.
- **Native UI (no repo precedent):** build LookAndFeel + Knob first; FlexBox layout; fallback to fixed-size UI for v1 if resizable is fiddly.
- **Parameter count discrepancy (41 vs 42 Float):** reconcile with the spec author before Stage 1 sign-off; do not invent a parameter.
- **Locked-spec vs brief distortion order:** parameter-spec order (`tanh→cubic→asym→softclip→hardclip→foldback→bitcrush`) is authoritative.

---

## Open Questions — RESOLVED in Stage 0 (see `architecture.md` Architecture Decisions)

| # | Question | Resolution |
|---|---|---|
| 1 | Default oversampling 2× vs 4× | **`2x`** provisional (choice index 1); confirmed at repo Stage 17 profiling. |
| 2 | Safety limiter design | **Zero-latency soft-clip ceiling −0.5 dBFS** as `limiter = on`. Short-lookahead true limiter **NOT in v1** (not in locked param list); internal hook `kLimiterLookaheadSamples = 0` for a future spec bump. |
| 3 | Distortion morph ordering + makeup | Locked order `tanh→cubic→asymmetric→soft-clip→hard-clip→foldback→bitcrush` (parameter-spec authoritative); equal-gain linear crossfade; **adaptive short-window RMS makeup** seeded by a static prime table (static 2-D LUT = fallback). Transfer functions specified in `architecture.md`. |
| 4 | Tone position | **Fixed pre-distortion** (not switchable — no parameter). |
| 5 | Pitch envelope contour | **Single `pitchCurve`-driven normalised-exponential snap+settle**, `k = 0.6 + pitchCurve·8.4`; `f(t) = fundamentalEff·pitchStartEff^{e(τ)}`; phase-integrated. Two-stage = optional Stage-2 refinement. |
| 6 | Mono crossover frequency | **Fixed 130 Hz** Linkwitz-Riley split (cannot expose — not in param list). |
| 7 | Click generator blend | **0.50 filtered-noise burst + 0.35 transient osc (with pitch drop) + 0.15 windowed raised-cosine impulse**; generated at base rate, band-limited by construction; move inside OS block only if aliasing measured at high `drive`. |
| 8 | `fundamental` in MIDI Pitch mode | **Global semitone offset** `12·log2(fundamental/55)` on top of the note (0 at the 55 Hz default); absolute in Fixed Frequency mode. Keeps the knob and `macroBody` meaningful. |
| 9 | `bodyAttack` | **Kept fixed-fast internal** (`kBodyAttackMs = 2.0`, raised-cosine). Not exposed (not in locked list). Soft-attack use case covered by negative `transientAttack`. Accepted deviation from the original spec's 0–20 ms amp attack. |
| 10 | `mix` | **Kept automatable** (locked contract) as an **equal-power blend processed↔silence** (no dry path for an output-only instrument); functions as an automatable fade/mute. Tooltip: "leave at 100 %". |

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
