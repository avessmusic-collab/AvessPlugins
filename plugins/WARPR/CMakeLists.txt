cmake_minimum_required(VERSION 3.15)

# ============================================================================
# WARPR (working title) - sample-based wave-warp synthesizer.
# Instrument (IS_SYNTH), MIDI in, stereo out (no input bus), WebView UI.
#
# The repo-root CMakeLists.txt already does project() + add_subdirectory(JUCE) and
# auto-discovers plugins/*, so there is no project()/JUCE here.
#
# Based on .ideas/mockups/v9-CMakeLists.txt.
# juce8-critical-patterns satisfied: #1 juce_generate_juce_header, #3 juce_gui_extra +
# JUCE_WEB_BROWSER=1, #9 NEEDS_WEB_BROWSER, #13 check_native_interop.js embedded,
# #22 IS_SYNTH.
#
# PLUGIN_CODE / PLUGIN_MANUFACTURER_CODE: "Void" / "Vstf" are FINAL (A21 resolved)
# (architecture A21), pending the final product name. They MUST be settled before the
# first public build: changing them later orphans saved DAW projects. Product-name
# strings used by C++ live in Source/PluginIdentity.h - keep PRODUCT_NAME below in sync.
# Renaming: uninstall old bundles first (pattern #14).
# ============================================================================

# Plugin formats: VST3 + Standalone everywhere; AU is macOS-only.
set(WARPR_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND WARPR_FORMATS AU)
endif()

juce_add_plugin(WARPR
    VERSION 1.0.0
    COMPANY_NAME "VST Factory"
    PLUGIN_MANUFACTURER_CODE Vstf
    PLUGIN_CODE Void
    FORMATS ${WARPR_FORMATS}
    PRODUCT_NAME "VOID"
    IS_SYNTH TRUE                 # instrument: MIDI in, stereo audio out (pattern #22)
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    NEEDS_WEB_BROWSER TRUE        # required for VST3 to load (pattern #9)
    NEEDS_WEBVIEW2 TRUE           # bundles the WebView2 loader on Windows (ignored on macOS)
)

# Translation units shared by the plugin and the console test runner.
set(WARPR_CORE_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Parameters/ParameterLayout.cpp
    Source/Backend/UiModel.cpp
    Source/Backend/UiBackendImpl.cpp
    Source/State/StateIO.cpp
    # Phase D1: sample engine, voices, decimator
    Source/DSP/SampleData.cpp
    Source/DSP/SampleStore.cpp
    Source/DSP/HalfbandIIR.cpp
    Source/DSP/WarprVoice.cpp
    Source/DSP/WarprSynth.cpp
    # Phase D4: A/B transfer-curve shaper (model + JSON, preset library, LUT/antiderivative builder + engine)
    Source/DSP/ShaperCurve.cpp
    Source/DSP/ShaperPresetLibrary.cpp
    Source/DSP/ShaperEngine.cpp
    # Phase D5a: standard filter family (coefficient generators + analytic response, per-voice stage)
    Source/DSP/FilterDesign.cpp
    Source/DSP/FilterStage.cpp
)

target_sources(WARPR PRIVATE ${WARPR_CORE_SOURCES})

target_include_directories(WARPR
    PRIVATE
        Source
)

# Required JUCE modules
target_link_libraries(WARPR
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats      # WAV/AIFF decode, FLAC embedding
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp                # oversampling, filter design
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra          # WebBrowserComponent (pattern #3)
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Generate JuceHeader.h (JUCE 8 requirement, pattern #1: after target_link_libraries)
juce_generate_juce_header(WARPR)

# ---------------------------------------------------------------------------
# WebView UI resources.  Embedded by FILE NAME:
#   index.html -> BinaryData::index_html, js/juce/index.js -> BinaryData::index_js,
#   check_native_interop.js -> BinaryData::check_native_interop_js
#
# Source/ui/public/index.html is the production page (.ideas/mockups/v9-ui.html).
# It is staged from the mockup ONCE, at first configure, only if it does not exist yet
# (so later hand edits are never overwritten). From then on Source/ui/public/index.html
# is the file of record. Full UI wiring is Stage 3 (phase G1).
# ---------------------------------------------------------------------------
set(WARPR_UI_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/public")
if(NOT EXISTS "${WARPR_UI_PUBLIC}/index.html")
    set(WARPR_MOCKUP_HTML "${CMAKE_CURRENT_SOURCE_DIR}/.ideas/mockups/v9-ui.html")
    if(NOT EXISTS "${WARPR_MOCKUP_HTML}")
        message(FATAL_ERROR "WARPR: ${WARPR_UI_PUBLIC}/index.html is missing and the mockup source ${WARPR_MOCKUP_HTML} was not found")
    endif()
    message(STATUS "WARPR: staging ${WARPR_MOCKUP_HTML} -> ${WARPR_UI_PUBLIC}/index.html")
    configure_file("${WARPR_MOCKUP_HTML}" "${WARPR_UI_PUBLIC}/index.html" COPYONLY)
endif()

juce_add_binary_data(WARPR_UIResources
    SOURCES
        Source/ui/public/index.html
        Source/ui/public/js/juce/index.js
        Source/ui/public/js/juce/check_native_interop.js   # required (pattern #13)
)

target_link_libraries(WARPR
    PRIVATE
        WARPR_UIResources
)

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------
target_compile_definitions(WARPR
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=1
        JUCE_USE_WIN_WEBVIEW2=1      # Windows: WebView2 backend (resource-provider API); no effect on macOS
        # NEEDS_WEBVIEW2 links WebView2 STATICALLY, so JUCE must call the static entry point;
        # without this it looks for a missing WebView2Loader.dll and falls back to the legacy IE
        # control ("Navigation canceled" / white screen).
        JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
        JUCE_USE_CURL=0
)

# ---------------------------------------------------------------------------
# Console test runner (EXCLUDE_FROM_ALL; run:  cmake --build build --target WARPR_Tests)
# Offline renderer, allocation hook, NaN/DC/discontinuity scanners, FFT alias meter,
# parameter-table / state / cross-thread-primitive checks.
# ---------------------------------------------------------------------------
add_executable(WARPR_Tests EXCLUDE_FROM_ALL
    Source/Tests/RunTests.cpp
    Source/Tests/PhaseD1Tests.cpp
    Source/Tests/PhaseD2Tests.cpp
    Source/Tests/PhaseD3Tests.cpp
    Source/Tests/PhaseD4Tests.cpp
    Source/Tests/PhaseD5Tests.cpp
    Source/Tests/PhaseD7Tests.cpp
    Source/Tests/OfflineRender.cpp
    Source/Tests/AllocHook.cpp
    ${WARPR_CORE_SOURCES}
)
target_include_directories(WARPR_Tests PRIVATE Source)
target_link_libraries(WARPR_Tests
    PRIVATE
        WARPR_UIResources
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(WARPR_Tests
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
)

# The test harness stack-allocates whole processors (34 voices x 8 oscillators x 16-grain pools
# since D7a), so the default 8 MB main stack overflows in ___chkstk_darwin.  Match the 64 MB
# stack the Windows test exe already uses.
if(APPLE)
    target_link_options(WARPR_Tests PRIVATE "-Wl,-stack_size,0x4000000")
endif()

# Platform notes
#   macOS  : WKWebView (built in).  Re-sign installed bundles after install (pattern #27).
#   Windows: WebView2 runtime; CI needs JUCE_WEBVIEW2_PACKAGE_LOCATION = PARENT of the nuget package dir.
#   Linux  : not a target for v1.0.
