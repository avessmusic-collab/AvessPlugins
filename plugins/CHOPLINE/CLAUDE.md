# CHOPLINE — CLAUDE.md

Tempo-synced vocal chop / warp / glitch instrument. The authoritative contract
is `SPEC.md` (user-authored); the locked visual direction is the v17 mockup
(`.ideas/mockups/v17-ui.yaml`, see `.ideas/creative-brief.md` "UI Design — FINAL").
Work phase by phase (spec §11) and STOP after each phase for user approval.

**Current phase: 0 (Scaffold) — complete, awaiting approval.**

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

## Known issues / notes

- **auval not run locally**: validating an AU requires the component to be
  installed in `~/Library/Audio/Plug-Ins/Components`, and nothing is
  installed to system folders during development. CI installs into the
  (disposable) runner and runs `auval -v aumu Chpl Manu`.
- Local pluginval run is strictness 10 with GUI tests; CI adds
  `--skip-gui-tests` (headless runners).
- Phase 0 editor label is the only UI; it uses the first LookAndFeel colour
  tokens (paper / ink / label / accent / repeat) from the v17 mockup.

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
