# CHOPLINE — CLAUDE.md

Tempo-synced vocal chop / warp / glitch instrument. The authoritative contract
is `SPEC.md` (user-authored); the locked visual direction is the v17 mockup
(`.ideas/mockups/v17-ui.yaml`, see `.ideas/creative-brief.md` "UI Design — FINAL").
Work phase by phase (spec §11) and STOP after each phase for user approval.

**Current phase: 1 (Sampler) — complete, awaiting approval.**

## Build commands

All commands from `plugins/CHOPLINE/` (self-contained project — NOT built by
the monorepo root CMakeLists.txt):

```bash
# Configure (fetches JUCE 9.0.4 + Catch2 v3.16.1 on first run)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build everything (VST3, AU, Standalone, tests)
cmake --build build --config Release --parallel

# Run unit tests
./build/CHOPLINE_Tests_artefacts/Release/CHOPLINE_Tests
#   (path is ./build/CHOPLINE_Tests_artefacts/CHOPLINE_Tests on single-config
#    generators without per-config subdirs — use whichever exists)

# pluginval (strictness 10, spec §12)
/Applications/pluginval.app/Contents/MacOS/pluginval \
  --strictness-level 10 --validate \
  "build/CHOPLINE_artefacts/Release/VST3/CHOPLINE.vst3"
```

Built artefacts land in `build/CHOPLINE_artefacts/Release/{VST3,AU,Standalone}`.
Do NOT install to system plugin folders until the user asks.

## Decisions made (Phase 0)

- **JUCE 9.0.4** — latest stable JUCE 9 tag at scaffold time (2026-10-24),
  pinned by exact tag via CMake **FetchContent** (chosen over CPM to avoid
  vendoring an extra script; over submodules to keep the monorepo clean).
  The system JUCE 8.0.15 at /Applications/JUCE is NOT used for CHOPLINE.
- **Catch2 v3.16.1** — pinned via FetchContent, test-only.
- **Plugin identity**: `PLUGIN_CODE Chpl`, `PLUGIN_MANUFACTURER_CODE Manu`,
  `COMPANY_NAME "CHOPLINE"` — follows the house de-brand convention (neutral
  placeholder manufacturer code, same as KICKR); the user supplies a real
  company name/code before public release.
- **C++20**, formats VST3 + AU + Standalone, IS_SYNTH, NEEDS_MIDI_INPUT,
  stereo output only (no input buses; sidechain capture bus is reserved for
  Phase 6).
- **Warnings as errors on project sources only** (`-Werror` / `/WX` applied
  per-source-file), because JUCE's CMake compiles JUCE module translation
  units into the same targets and those must not be gated by our -Werror.
- **Namespaces**: `chopline::engine` (DSP, no GUI), `chopline::params` /
  `chopline::state` (APVTS + models), `chopline::ui` (LookAndFeel, views).
- **Test target** (`CHOPLINE_Tests`, juce_add_console_app + Catch2WithMain)
  compiles only engine/state sources — none of our UI/editor code. Note:
  `juce_audio_processors` transitively pulls juce_gui_extra/juce_gui_basics
  as JUCE *module* dependencies; that is JUCE's module graph, not a GUI
  dependency of the engine.
- **State versioning from day one**: `chopline::params::kSchemaVersion` (=1)
  stored as `schemaVersion` on the APVTS root in getStateInformation.
- **Universal binary (arm64 + x86_64)** deferred to release packaging
  (Phase 6): local + CI builds are native-arch for speed. Enable later with
  `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.
- **CI**: `.github/workflows/chopline.yml` (self-contained; monorepo-root
  workflows untouched). macOS + Windows matrix, Release build, Catch2 tests,
  pluginval strictness 10. Follows the house pluginval pattern (see
  prism-windows.yml): on Windows pluginval.exe is a GUI-subsystem binary —
  pipe to force the wait and judge by the printed verdict, not the exit code;
  pwsh masks non-final exit codes, so check `$LASTEXITCODE` after every build
  step.

## Decisions made (Phase 1 — Sampler)

- **SENS is mode-dependent** (locked v17 "lean slicing controls" = one knob):
  Transient → onset sensitivity; Grid → division of the sample BPM
  (1/4·1/8·1/16·1/32 by quartile); Equal → slice count 2–64; Manual → unused.
- **Slicing controls are ValueTree STATE, not APVTS** (v17 scope `slice_map`):
  mode, SENS, min slice length, zero-cross snap, sample BPM and the markers
  live in `SLICES`/`SAMPLE` children of the root. The only Phase 1 APVTS
  parameter is `midiBaseNote` (choice C0–C3, default C1 = 36, Ableton naming).
- **Markers are stored in seconds** (model, message thread); the audio thread
  sees a `SliceMapSnapshot` in sample frames. A session-rate change re-resamples
  in the background and the slice map survives untouched.
- **Snapshot reclamation**: `SnapshotExchange` publishes generation-stamped
  nodes; the audio thread acks the generation at block end; the message thread
  frees a retired snapshot only when a NEWER generation was acked AND no voice
  still reads it (per-stream atomic sample pointers). Consequence: loading a
  new sample never cuts ringing voices — they finish on the old snapshot.
- **Voice stealing**: a retrigger moves the running stream into an in-voice
  2 ms fast-fading TAIL and starts the new note immediately — "steal oldest
  with 2 ms fade" without delaying the new onset. 16 voices preallocated.
- **Declick**: equal-power (sin) fades at chop start/end, per-chop 0–20 ms,
  default 2 ms, applied at playback (never destructive), clamped to half the
  chop length.
- **FLAC blob cached at load** (24-bit, lossless-for-purpose): used for state
  embedding (`embed` toggle in settings, default ON) and as the source for
  session-rate re-resampling (original file read exactly once). Restore
  prefers the embedded blob, falls back to path; path + MD5 hash stored for
  the Phase-later relink dialog (spec §10).
- **Note-off is ignored in Phase 1** (one-shot chop playback). Gate/Loop/Fit
  playback modes and choke groups are Phase 3 per spec §6/§11.
- **Strip A interactions** (per the Phase 1 brief): wheel zoom + scroll,
  draggable markers in ALL modes, double-click add / alt-click delete in
  Manual, click-to-audition, shift-click multi-select. NOTE: the v14/v17 YAML
  says the strip is "exactly v12" with editing only in the chop overlay — the
  overlay sample editor is a later phase; reconcile when it lands.
- **Sample BPM** is detected at load (inter-onset-interval histogram, fallback
  120) and user-editable in the '…' slicing flyout (spec §4 Grid mode).
- **juce_cryptography** linked (MD5 file hash). JUCE 9 writer API:
  `AudioFormatWriterOptions` (the old `createWriterFor` overload is deprecated
  and -Werror fails on it).
- **setStateInformation hops to the message thread** when called off-thread
  (pluginval strictness 10 does this); `getStateInformation` is thread-safe via
  the engine's model lock.
- **Dev-only hooks** (Standalone builds, env vars, no effect in hosts):
  `CHOPLINE_AUTOLOAD_SAMPLE=<path>` loads a file at startup;
  `CHOPLINE_UI_SNAPSHOT=<path.png>` renders the editor to PNG ~6 s after open
  (component snapshot — not a screen capture). Tests: hidden `[.smoke]` case
  uses `CHOPLINE_SMOKE_FILE` / `CHOPLINE_SMOKE_OUT` for a real-vocal offline
  render; never run by default/CI, no user paths committed.
- **Allocation discipline is enforced by test**: `EngineTests.cpp` replaces
  global new/delete in the test binary and asserts zero (de)allocations on the
  calling thread across 1000 processed blocks with triggers (spec §8/§12).

## Known issues / notes

- **auval not run locally**: validating an AU requires the component to be
  installed in `~/Library/Audio/Plug-Ins/Components`, and nothing is
  installed to system folders during development. CI installs into the
  (disposable) runner and runs `auval -v aumu Chpl Manu`.
- Local pluginval run is strictness 10 with GUI tests; CI adds
  `--skip-gui-tests` (headless runners).
- **Sample + slice snapshots publish independently** — after a load there is a
  one-block window where a trigger can pair the new sample with the previous
  slice snapshot. Voice bounds-clamping makes it safe (no crash/overread);
  revisit with a shared serial if it ever becomes audible.
- **macOS TCC in the Standalone**: loading a file from a protected folder
  (Desktop/Documents) via a path the app did not get from a file chooser or
  drag can block on the system permission prompt (found via the dev autoload
  hook). Normal UI loads (chooser / drag-and-drop) carry consent and are fine.
- **During a session-rate change**, ringing voices finish on the old-rate
  snapshot (brief pitch offset); in practice `prepareToPlay` resets voices.
- **Transient defaults by ear pending**: default SENS 55 / min 80 ms gave 9
  chops on the 34 s dev vocal — musically plausible but tune with the user in
  DAW testing (Phase 1 user test).
- Waveform repaints the whole display on animated frames (playheads); fine at
  60 fps on M-series — sub-region dirty rects are a Phase 5 polish item.

## Open questions

- **JUCE licence tier**: confirm the JUCE 9 revenue-cap tier and buy the
  right licence before any public release (see THIRD_PARTY_LICENSES.md).
- **DECIDED (record only)** — master-only filter with fixed 12 dB/oct slope:
  spec §6 lists a per-chop state-variable filter with 12/24 dB slopes; the
  locked v17 design supersedes it (filter is master-only, HP removed,
  12 dB/oct fixed; per-chop distortion also removed in favour of a 6-type
  master distortion absorbing saturation; tilt/tone removed). Build phases 3–4
  follow v17, not spec §6/§7, where they conflict.
- Spec asks for SPEC.md "in the repo root"; in this monorepo it lives at
  `plugins/CHOPLINE/SPEC.md` (the plugin's own root) — flag if a copy at the
  monorepo root is wanted.
