---
plugin: Transitionist
date: 2026-08-26
problem_type: build_error
component: system
symptoms:
  - "Standalone app shows old UI after rebuild + reinstall reports success"
  - "VST3/AU install correctly reflects latest GUI changes, Standalone does not"
  - "Standalone binary timestamp far older than VST3/AU binary timestamps after a 'successful' build"
root_cause: logic_error
resolution_type: code_fix
severity: moderate
tags: [build, standalone, webview, script, scripts/build-and-install.sh]
---

# scripts/build-and-install.sh Never Rebuilt the Standalone Target

## Problem

After making several rounds of WebView UI changes (outer-frame recolor, a new
knob interaction behavior, a new preset-bar feature) and running
`./scripts/build-and-install.sh Transitionist` successfully each time (all 7
phases green, VST3/AU installed with fresh timestamps), reopening the
Standalone `.app` for manual visual testing kept showing UI from hours
earlier - missing every single one of those changes, despite the script
reporting success every time.

## Investigation

- Confirmed `Source/ui/public/index.html` on disk had all the expected
  changes (`grep` for the new markup/JS succeeded).
- Confirmed the compiled `BinaryData*.cpp` for the **VST3** artifact
  contained the new HTML/JS (`grep` on
  `build/plugins/Transitionist/juce_binarydata_.../BinaryData1.cpp`).
- Took a screenshot of the running Standalone app: UI was missing the latest
  changes even though the VST3/AU install had just completed.
- Compared binary timestamps directly:

  ```bash
  stat -f "%Sm %N" build/plugins/Transitionist/Transitionist_artefacts/Release/VST3/Transitionist.vst3/Contents/MacOS/Transitionist
  # Aug 26 15:47:23 2026

  stat -f "%Sm %N" build/plugins/Transitionist/Transitionist_artefacts/Release/Standalone/Transitionist.app/Contents/MacOS/Transitionist
  # Aug 26 10:59:54 2026   <- from the ORIGINAL Stage 3 Phase 5.1 build, hours earlier
  ```

## Root Cause

`scripts/build-and-install.sh` Phase 2 only ever built two CMake targets:

```bash
# BEFORE (bug)
cmake --build "$build_dir" --config Release \
  --target "${PLUGIN_NAME}_VST3" --target "${PLUGIN_NAME}_AU" --parallel
```

Every plugin in this codebase declares `FORMATS VST3 AU Standalone` in
`CMakeLists.txt` (verified across all 13 plugins with a WebView UI), but the
`_Standalone` target was never included in the build invocation, and Phase 5
(install) never touches Standalone either - it only copies VST3/AU into
`~/Library/Audio/Plug-Ins/`. The Standalone `.app` sitting in
`build/plugins/<Name>/<Name>_artefacts/Release/Standalone/` is left
completely untouched by the script, forever frozen at whatever its last
manual/explicit build was.

This is easy to miss because:
- The script reports `✓ Build and installation complete!` - nothing
  indicates Standalone was skipped.
- VST3/AU (the formats the script *does* handle) are correct, so a
  DAW-based test would have caught the real, current state.
- Standalone is the natural "no DAW required" way to manually eyeball
  WebView changes, so it's exactly the artifact most likely to get opened
  for a quick visual check - and exactly the one silently going stale.

## Solution

Added `--target "${PLUGIN_NAME}_Standalone"` to the same build invocation:

```bash
# AFTER (fixed)
cmake --build "$build_dir" --config Release \
  --target "${PLUGIN_NAME}_VST3" \
  --target "${PLUGIN_NAME}_AU" \
  --target "${PLUGIN_NAME}_Standalone" \
  --parallel
```

Since most compilation units (DSP, shared JUCE modules, BinaryData) are
already shared across all three target formats, adding Standalone to the
same `--parallel` build only adds one extra link step - negligible time cost
for guaranteed freshness.

**One-off manual fix** (needed once, for any plugin already affected):

```bash
cmake --build build --config Release --target <PluginName>_Standalone --parallel
```

## Verification

```bash
stat -f "%Sm %N" build/plugins/Transitionist/Transitionist_artefacts/Release/Standalone/Transitionist.app/Contents/MacOS/Transitionist
# Timestamp should now match (or postdate) the VST3/AU binaries from the same build run
```

Confirmed via screenshot: after the targeted `_Standalone` rebuild, the
Standalone app correctly showed the preset bar and all prior UI changes.

## Prevention

**RESOLVED: 2026-08-26** - `scripts/build-and-install.sh` Phase 2 now always
builds `_VST3` + `_AU` + `_Standalone` together.

**General principle for this codebase:** if a build script targets a subset
of a plugin's declared `FORMATS`, any format left out will silently drift
from source with no error signal - the script's own "success" message gives
false confidence. When adding/changing which formats a script builds,
cross-check against `grep "^\s*FORMATS" plugins/*/CMakeLists.txt` to confirm
every declared format is actually covered somewhere in the pipeline (built,
or explicitly and visibly documented as build-only-on-demand).

## Related Issues

- See also: [stale-build-after-stage-5-gui-changes-OrganicHats-20251112.md](stale-build-after-stage-5-gui-changes-OrganicHats-20251112.md) -
  same *symptom family* (stale installed binary after GUI changes) but a
  different root cause (validator skipped rebuild entirely, vs. this issue
  where the build script itself never targeted Standalone in the first
  place).
