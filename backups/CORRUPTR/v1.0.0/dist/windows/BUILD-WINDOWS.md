# Building CORRUPTR for Windows

The Windows binaries must be compiled ON Windows (JUCE requires MSVC;
cross-compiling from macOS is not supported).

## One-time setup (Windows machine or VM)
1. Install Visual Studio 2022 (Desktop development with C++) + CMake.
2. Install JUCE 8 to C:\JUCE (or adjust the path below to your global
   JUCE location as referenced by the repo's top-level CMakeLists).
3. Clone/copy this repository.

## Build
```bat
cmake -B build-win -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --config Release --target CORRUPTR_VST3 CORRUPTR_Standalone
```
The CMake config is already platform-aware (AU is skipped off-macOS;
NEEDS_WEBVIEW2 bundles the WebView2 loader for the UI).

Artifacts land in:
`build-win/plugins/CORRUPTR/CORRUPTR_artefacts/Release/`

## Package
1. Install Inno Setup 6 (https://jrsoftware.org/isinfo.php).
2. Open `plugins/CORRUPTR/dist/windows/CORRUPTR.iss` and compile
   (adjust the two Source paths if your build folder differs).
3. Output: `CORRUPTR-1.0.0-Windows.exe` - a wizard installer with
   VST3 (always) + Standalone (optional) components, installing the
   VST3 to the standard `C:\Program Files\Common Files\VST3`.

## Sanity checklist before shipping
- Load the VST3 in a Windows DAW; verify the WebView UI appears
  (WebView2 runtime present) and presets apply.
- Run pluginval for Windows (strictness 10) on the built VST3.

## Alternative: build in the cloud
A GitHub Actions workflow on windows-latest can produce these
binaries automatically on every push - ask Claude to set one up if
the repo gets a GitHub remote.
