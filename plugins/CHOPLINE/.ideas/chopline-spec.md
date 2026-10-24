# CHOPLINE — Build Spec for Claude Code

Read this whole spec first, save it as SPEC.md in the repo root, then start Phase 0. Work one phase at a time and stop after each phase for my approval.

## 1. Role, mission and product vision

You are a senior audio DSP and JUCE engineer building a commercial-grade plugin. I am a producer with 10 years of experience (melodic techno, IDM, UK garage). I own creative direction and sound; you own the code, DSP correctness, real-time safety and build system. Push back when a request would hurt audio quality, stability or CPU, and propose the better option.

**What CHOPLINE is:** a tempo-synced vocal chop, warp and glitch instrument. I drop in a vocal sample, slice it into chops, then arrange those chops on a beat grid that plays in time with the host. Each chop can be warped, pitched, distorted, reversed and stuttered. A beat-repeat stage on the output adds live, performable glitches on top.

**Two strips, top to bottom:**

1. **Strip A — Sampler.** Waveform of the loaded sample with slice markers. Chops are auditionable and draggable.
2. **Strip B — Grid.** A one- to four-bar step lane with selectable resolution: 1/4, 1/8, 1/16, 1/32 (plus triplet and dotted variants). I drag chops from Strip A onto grid cells to build the pattern.

**Reference points (for feel, not to copy):** Ableton Simpler slice mode, Ableton Beat Repeat, Audio Damage Replicant 3, Cymatics Relapse, W.A. Production ChopBeast, FL Studio Slicex. CHOPLINE's angle is the combination: a chop arranger plus deep per-chop warp/glitch processing in one window, aimed at vocals.

**Quality bar:** it must feel like a paid plugin from a serious indie developer. No clicks at slice boundaries, sample-accurate timing, no allocation on the audio thread, smooth resizable UI, full automation, reliable state recall.

## 2. Tech stack, licensing and hard constraints

Use JUCE 9 with CMake and C++20, latest stable JUCE tag at build time, pinned as a git submodule or CPM dependency.

| Area | Decision |
| --- | --- |
| Framework | JUCE 9 via CMake (`juce_add_plugin`). No Projucer. |
| Formats | VST3, AU, Standalone. AAX and CLAP later; keep the code format-agnostic. |
| Platforms | macOS universal binary (arm64 + x86_64), Windows x64. |
| Plugin type | Instrument (synth) with MIDI input and stereo output. Optional sidechain input bus reserved for Phase 6 live capture. |
| Time-stretch / pitch | Signalsmith Stretch (MIT, header-only, real-time capable, formant options): https://github.com/Signalsmith-Audio/signalsmith-stretch |
| Not allowed | Rubber Band (GPL unless a commercial licence is bought). No GPL/LGPL code at all. |
| Audio files | JUCE `AudioFormatManager`: WAV, AIFF, FLAC, MP3 (decode), OGG. |
| Resampling | Windowed-sinc or polyphase (8–16 taps), cheaper mode for live, high-quality mode for render. |
| Tests | Catch2 v3 for DSP unit tests; pluginval at strictness 10 in CI. |
| CI | GitHub Actions matrix: macOS + Windows builds, tests, pluginval. |
| Licensing / copy protection | Clean `LicenseManager` interface stub only; no implementation yet. |

**Hard rules**

- Every third-party dependency must be MIT, BSD, zlib, Apache-2.0 or the JUCE licence. List each in `THIRD_PARTY_LICENSES.md`.
- Nothing on the audio thread allocates, locks, logs, does file I/O or waits on the message thread.
- Sample-rate independent: 44.1 to 192 kHz, any buffer size from 16 to 4096, including variable block sizes.
- Correct behaviour with transport stopped, looping, scrubbing and offline bounce.
- Keep a `CLAUDE.md` with build commands, decisions made, known issues and open questions.

## 3. Architecture and source layout

Separate DSP from UI completely. The engine must build and run in unit tests with no GUI module linked.

**Signal flow per block**

1. Read host position (PPQ, BPM, playing, loop) from `AudioPlayHead`.
2. `PatternScheduler` finds every grid step that starts inside this block, at sample-accurate offsets. MIDI notes that trigger chops are merged in.
3. Each trigger allocates a `ChopVoice` from a fixed pool (16 voices, steal oldest with a 2 ms fade).
4. Each voice: read source → warp/stretch → pitch → per-chop filter → per-chop distortion → amp envelope + declick → pan.
5. Voices sum to the chop bus → Beat Repeat → Master FX (tone, saturation, width, limiter) → output.

**Threads**

- Audio thread: engine only, reads immutable snapshots.
- Message thread: UI. Edits produce a new `PatternSnapshot` / `SliceMapSnapshot`, published via lock-free single-producer handoff (atomic pointer swap; old snapshot freed on the message thread via a release queue).
- Background thread: file loading, decoding, resampling to session rate, transient analysis, waveform peak cache, offline pre-stretch.

**Source layout**

    CHOPLINE/
      CMakeLists.txt
      CLAUDE.md
      SPEC.md
      THIRD_PARTY_LICENSES.md
      libs/                       # JUCE, signalsmith-stretch, Catch2 (submodules)
      src/
        PluginProcessor.{h,cpp}
        PluginEditor.{h,cpp}
        engine/
          Engine.{h,cpp}            # owns everything below, processBlock entry
          SampleStore.{h,cpp}       # decoded audio, ref-counted, swap-safe
          SliceMap.{h,cpp}          # slice points + per-chop settings
          TransientDetector.{h,cpp}
          PatternScheduler.{h,cpp}  # grid -> sample-accurate triggers
          ChopVoice.{h,cpp}
          VoicePool.{h,cpp}
          WarpEngine.{h,cpp}        # varispeed, stretch, granular, freeze
          Distortion.{h,cpp}        # oversampled shapers, crusher
          BeatRepeat.{h,cpp}
          MasterFX.{h,cpp}
          Smoothers.h, Envelopes.h, Interpolators.h
        state/
          Parameters.{h,cpp}        # APVTS layout, IDs, ranges
          PatternModel.{h,cpp}      # ValueTree <-> snapshot
          PresetManager.{h,cpp}
        ui/
          LookAndFeel.{h,cpp}
          SamplerStrip.{h,cpp}
          GridStrip.{h,cpp}
          ChopInspector.{h,cpp}
          BeatRepeatPanel.{h,cpp}
          MasterPanel.{h,cpp}
          WaveformCache.{h,cpp}
      tests/
      presets/

## 4. Strip A: sampler and slicer

Strip A loads one sample, shows it, and turns it into up to 64 chops that can be auditioned and dragged onto the grid.

**Loading**

- Drag-and-drop from Finder/Explorer or the DAW, plus a Load button. Max length 5 minutes stereo.
- Decode and resample to session rate on the background thread; show a spinner, never block the UI.
- Store the file path and also embed the audio in plugin state (compressed FLAC, toggle in settings) so projects open on other machines.
- Optional: detect sample BPM and root key, editable by the user.

**Slicing modes**

| Mode | Behaviour |
| --- | --- |
| Transient | Spectral-flux onset detection tuned for vocals (syllable onsets). Sensitivity knob plus minimum slice length in ms. |
| Grid | Equal divisions synced to the sample's BPM: 1/4, 1/8, 1/16, 1/32. |
| Equal | N equal slices (2–64). |
| Manual | Double-click to add a marker, drag to move, right-click or Alt-click to delete. |

All modes create markers that can then be edited manually. Markers snap to nearest zero crossing (toggle). Each chop gets a colour and an index label.

**Interaction**

- Click a chop to audition it (through the full per-chop FX chain). Shift-click to select several.
- Zoom (mouse wheel / pinch) and scroll; mini-map overview above the zoomed view.
- Chops are drag sources: drag one or a multi-selection onto Strip B.
- MIDI: chop N plays from MIDI note C1 + N (base note configurable), velocity-sensitive.
- Per-chop start/end fine-trim handles inside the selected chop.

**Declick:** every chop start and end gets a short equal-power fade (0–20 ms, default 2 ms), applied at playback, never destructively.

## 5. Strip B: grid sequencer 1/4–1/32

Strip B is a host-synced step lane where chops are placed, stretched across steps and given per-step variations.

**Grid**

- Resolution: 1/4, 1/8, 1/16, 1/32, each Straight / Triplet / Dotted. Default 1/16.
- Pattern length: 1, 2 or 4 bars (odd step counts for polymeter in Phase 5).
- Changing resolution keeps events at their musical positions; off-grid events are kept and shown until moved.
- Swing 0–75% on even steps; applies to trigger time, not chop audio.
- 8 pattern slots (A–H), switchable live ("at bar end" or "now"), plus MIDI note switching.

**Placing chops**

- Drag a chop from Strip A onto a cell. A multi-selection places chops in order on consecutive cells.
- Drag an event's right edge to span several steps (chop keeps playing or loops, per its playback mode).
- With a chop selected, click an empty cell to paint it; drag across cells to paint a run.
- Alt-drag duplicates, Delete removes, Ctrl/Cmd-Z undo/redo (`juce::UndoManager`, 100 steps).
- Events show chop colour, index and a tiny waveform.

**Per-step overrides** (lane editor under the grid)

| Lane | Range |
| --- | --- |
| Velocity / gain | 0–127 |
| Pitch | −24 to +24 st, fine ±100 cents |
| Reverse | on / off |
| Ratchet (retrigger) | 1, 2, 3, 4, 6, 8 repeats per step, with decay |
| Probability | 0–100% |
| Stretch amount | 0.25× to 4× |
| Drive | 0–100% |
| Filter cutoff | 20 Hz–20 kHz |
| Pan | L100–R100 |

**Generators:** Randomize (chop order, pitch, reverse, ratchet) with Seed and Amount so results are repeatable; shift pattern left/right; reverse pattern; copy/paste between slots.

## 6. Per-chop warp, detune, distortion and glitch engine

Every chop has its own settings, edited in the Chop Inspector. Grid steps can override them.

**Playback modes**

| Mode | What it does |
| --- | --- |
| One-shot | Plays start to end, ignores step length. |
| Gate | Plays for the event length, then releases. |
| Loop | Loops (forward or ping-pong) for the event length, crossfaded loop point. |
| Fit to step | Time-stretches the chop to exactly fill the event length, pitch preserved. |

**Warp modes**

1. **Varispeed / tape** — speed and pitch together, ±24 st, optional tape-stop and tape-start curves (ms or note length).
2. **Stretch** — Signalsmith Stretch: independent time 0.25×–4× and pitch ±24 st, formant preserve on/off, formant shift ±12 st.
3. **Granular** — grain size 5–200 ms, density, position jitter, pitch jitter, spray.
4. **Freeze** — holds one grain or spectral frame at a chosen position for the event length.

Use a separate stretcher per active voice; pre-roll with `inputLatency()` samples so the chop starts on time. Where stretch latency cannot be hidden, pre-render that chop offline on the background thread when its settings change and play the rendered buffer, falling back to live processing while the render runs.

**Detune and pitch**

- Coarse ±24 st, fine ±100 cents, all smoothed.
- Pitch envelope: start offset ±24 st, glide time, curve.
- Unison: 1–4 copies, spread up to 50 cents, stereo width.
- Optional scale lock to a chosen key/scale.

**Distortion** (per chop, 2×–8× oversampling via `juce::dsp::Oversampling`, latency reported)

| Type | Notes |
| --- | --- |
| Soft clip | tanh, warm |
| Hard clip | drive and ceiling |
| Tape | asymmetric, slight compression |
| Fold | wavefolder, aggressive |
| Bitcrush | bit depth 1–16 |
| Downsample | rate reduction, no oversampling (aliasing intended) |

Controls: Drive, Tone (tilt), Mix, Output gain with auto-gain toggle.

**Other per-chop controls:** Reverse, Gain, Pan, AHDSR amp envelope, state-variable filter (LP/BP/HP, 12/24 dB, cutoff, resonance, envelope amount), Choke group (default: all chops in group 1 for classic vocal-chop behaviour).

## 7. Beat repeat and master FX

Beat Repeat captures the plugin's own output and repeats it in time.

| Parameter | Range / options |
| --- | --- |
| Mode | Auto (probability), Manual (hold button / MIDI note), Off |
| Interval | 1/4 bar to 4 bars |
| Offset | 16 steps |
| Chance | 0–100% |
| Grid (repeat size) | 1/2 to 1/64, straight/triplet, optional random range |
| Gate | 1/16 to 4 bars |
| Decay | volume drop per repeat 0–100% |
| Pitch decay | 0–12 st lower per repeat |
| Stutter accel | repeat size shrinks over the gate (roll/build-up) |
| Reverse repeats | off / alternate / all |
| Filter | LP/HP on repeats, sweep over the gate |
| Mix | Mix, Insert (mute dry while repeating), Gate (repeats only) |
| Freeze | holds the captured slice while held |

Capture into a pre-allocated ring buffer (4 bars at 30 BPM at 192 kHz). Crossfade every repeat boundary (1–10 ms). MIDI performance notes: one per grid size (C3 = 1/4, C#3 = 1/8, …).

**Master FX** (after Beat Repeat)

1. Tilt EQ / tone and high-pass (20–500 Hz).
2. Global tape-style saturation, oversampled.
3. Stereo width (mid/side, mono below a set frequency).
4. Tempo-synced delay and small plate reverb send for chop tails.
5. Output gain and transparent safety limiter (default on, −0.3 dBFS).

Per-section bypass and global Output.

## 8. Timing, sync and real-time safety

**Sync**

- Derive step times from host PPQ every block, never from a drifting internal counter. Handle tempo changes mid-block.
- Transport stopped: pattern stops (standalone: "free run" at manual BPM). MIDI chop triggers still play.
- Loop/jumps: detect PPQ discontinuities, release voices with a short fade, resume at the correct step.
- No PPQ from host: internal BPM with tap tempo.
- Report total latency via `setLatencySamples`, constant while playing.
- Offline render (`isNonRealtime()`): high-quality resampling and stretch.

**Real-time rules**

- All buffers, voices, stretchers, ring buffers allocated in `prepareToPlay` or on the background thread.
- No `new`/`delete`, vector growth, `std::string`, mutex, allocating `std::function`, logging or file I/O in `processBlock`.
- Parameters via cached `std::atomic<float>*` from APVTS; smooth every audible parameter (10–50 ms).
- UI ↔ engine via immutable snapshots + atomic swap; meters/playhead back via lock-free FIFO.
- `ScopedNoDenormals`; guard against NaN/Inf in feedback paths.
- Debug-build allocation detector that asserts if the audio thread allocates.

**CPU target:** under 5% of one core on Apple M1 at 48 kHz / 256 samples with 8 voices, stretch mode, 2× oversampling and Beat Repeat on. Provide an "Eco" quality switch.

## 9. UI/UX spec

One window, dark theme, native JUCE components with a custom `LookAndFeel` (not WebView).

**Layout, top to bottom** (default 1100 × 720 px, resizable 75%–200%, fixed aspect, scale saved)

1. **Header** — logo, preset browser, pattern slots A–H, BPM readout, Eco/HQ, settings, CPU meter.
2. **Strip A: Sampler** — mini-map, zoomable waveform, markers, slicing controls, Load.
3. **Strip B: Grid** — resolution, length, swing, step lane, animated playhead, collapsible per-step lane editor (tabs: Velocity, Pitch, Ratchet, Probability, Stretch, Drive, Filter, Pan).
4. **Bottom row** — Chop Inspector · Beat Repeat · Master.

**Feel**

- Waveform from a multi-resolution peak cache; repaint dirty regions only; playhead at 60 fps via `VBlankAttachment`.
- Knobs: drag, Shift fine-drag, double-click reset, value popup, right-click MIDI learn.
- Tooltips everywhere; "?" help overlay.
- Keyboard: Space preview (standalone), 1–8 pattern slots, Delete, undo/redo, arrows move events.
- Colour-blind-safe chop palette; WCAG AA text contrast.
- Design tokens file (colours, radii, fonts, spacing) for one-place restyling.

Ask me for visual direction before polishing the look in Phase 5.

## 10. Parameters, presets, state and export

**Automation**

- APVTS globals: Beat Repeat, Master FX, swing, pattern slot, quality, output.
- 8 Macros, each to up to 8 targets (any per-chop or global parameter) with range and curve.
- Stable parameter IDs with a version number from day one.

**State**

- Store APVTS, slice map, per-chop settings, 8 patterns, macros, UI scale, sample path, optional embedded FLAC.
- Versioned schema with migrations; unit-test every previous version.
- Missing sample: relink dialog, pattern kept.

**Presets**

- XML presets in user documents, categorised (Chops, Glitch, Stutter, Tape, Granular). "Kit only" or "full" (with sample).
- Ship 30 factory presets; I supply samples and tune them.

**Export**

- Drag-out MIDI of the current pattern.
- Drag-out audio: render N bars to WAV on the background thread, offered as a drag source.
- Export all chops as individual WAVs.

## 11. Build phases

At the end of each phase: builds on macOS and Windows, tests and pluginval pass, `CLAUDE.md` updated, then STOP and give me a short list of what to test by ear.

| Phase | Scope | I test |
| --- | --- | --- |
| 0. Scaffold | CMake, JUCE 9, formats, CI, Catch2, pluginval, empty editor, CLAUDE.md, licence files | Loads in Ableton/Logic/FL |
| 1. Sampler | Loading, waveform, all slicing modes, audition, MIDI trigger, declick, voice pool | Clean slices, no clicks |
| 2. Grid | Scheduler, host sync, resolutions + triplet/dotted, drag to grid, spans, swing, undo, state | Locked to DAW, loops correctly |
| 3. Chop FX | Playback modes, varispeed, stretch + formants, pitch env, unison, distortion, filter, envelope, choke | Warp/detune quality on vocals |
| 4. Beat Repeat + Master | Full Beat Repeat, MIDI performance notes, master chain, limiter | Glitches musical and in time |
| 5. Pro features | Per-step lanes, ratchets, probability, randomize, slots, macros, presets, granular + freeze, drag-out, UI polish | Workflow speed, presets |
| 6. Release prep | Live input capture (sidechain), AAX/CLAP evaluation, performance pass, signing/notarization scripts, installer, LicenseManager hook | Final listening + CPU |

Do not start a phase until I approve the previous one. If you find a design problem in this spec, note it in CLAUDE.md under "Open questions" and ask me.

## 12. Testing, QA and definition of done

**Automated tests (Catch2)**

- Scheduler: triggers on the exact sample for each resolution, swing, tempo change, loop wrap, block sizes 1–4096.
- Declick: no sample-to-sample jump above threshold at chop boundaries.
- Stretch: output length within 1 ms of requested ratio; 440 Hz sine shifted +7 st within 2 cents.
- Distortion: no NaN/Inf; aliasing below a set level at 2× on a 10 kHz sine.
- State: save → load round-trip identical; older schemas load.
- Real-time: allocation detector over 10 s of audio with all features on.

**pluginval** strictness 10 on every CI run.

**Manual DAW checklist** (Ableton Live, Logic Pro, FL Studio, Reaper, Bitwig)

- [ ] Loads and reopens projects with sample and patterns intact
- [ ] Stays in sync for 10 minutes with loop, tempo automation, scrubbing
- [ ] Offline bounce matches real-time
- [ ] No clicks, dropouts or denormal spikes; CPU within target
- [ ] Automation, MIDI learn, undo/redo reliable
- [ ] UI clean at 75/100/150/200% and HiDPI Windows
- [ ] Drag-out MIDI and audio work in each DAW

**Code quality:** warnings as errors, clang-format, no raw `new`, small documented classes, a header comment in every DSP file explaining its algorithm.

Start now with Phase 0.
