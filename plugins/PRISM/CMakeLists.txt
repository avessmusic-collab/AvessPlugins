cmake_minimum_required(VERSION 3.15)

# PRISM - bit-transparent multi-source analyzer (audio effect: stereo in/out +
# optional sidechain input; no MIDI). Adapted from
# .ideas/mockups/v8-CMakeLists.txt. JUCE is added once at the repo-root
# CMakeLists.txt; this file is auto-discovered (no project()/add_subdirectory(JUCE)).
set(PRISM_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND PRISM_FORMATS AU)
endif()

juce_add_plugin(PRISM
    COMPANY_NAME "PluginFreedom"
    PLUGIN_MANUFACTURER_CODE Plgf
    PLUGIN_CODE Prsm
    FORMATS ${PRISM_FORMATS}
    PRODUCT_NAME "PRISM"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    NEEDS_WEB_BROWSER TRUE      # Pattern #9 (VST3 WebView); Stage 3 uses it
    NEEDS_WEBVIEW2 TRUE         # Windows WebView2 backend; ignored elsewhere
)

# Translation units shared by the plugin and the console test runner.
set(PRISM_CORE_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    # Stage 2 Phase 2.1 - capture, worker skeleton, snapshot publishing
    Source/Analysis/AnalysisEngine.cpp
    Source/Analysis/AnalysisWorker.cpp
    # Stage 2 Phase 2.3 - spectrum engine
    Source/Analysis/SpectrumEngine.cpp
    # Stage 2 Phase 2.4 - loudness (K-weighting, gated M/S/I, LRA)
    Source/Analysis/LoudnessMeter.cpp
)

target_sources(PRISM PRIVATE ${PRISM_CORE_SOURCES})

target_include_directories(PRISM
    PRIVATE
        Source
)

target_compile_features(PRISM PRIVATE cxx_std_20)

target_link_libraries(PRISM
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp              # juce::dsp::FFT (Stage 2)
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra        # WebBrowserComponent (Stage 3)
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Pattern #1: after target_link_libraries(), before compile definitions.
juce_generate_juce_header(PRISM)

# Debug-only switches (OFF by default): -DPRISM_FRAME_TIMING=ON logs pushAnalysisFrame timing to stderr,
# -DPRISM_DEBUG_TONE=ON analyses a synthetic 1 kHz / -12 dBFS stereo sine instead of the input.
option(PRISM_FRAME_TIMING "PRISM: log editor frame-push timing" OFF)
option(PRISM_DEBUG_TONE "PRISM: analyse a synthetic test tone (debug only)" OFF)
option(PRISM_DEBUG_CONTROL "PRISM: execute JS from $PRISM_DEBUG_JS_FILE (debug only)" OFF)
if(PRISM_DEBUG_CONTROL)
    target_compile_definitions(PRISM PUBLIC PRISM_DEBUG_CONTROL=1)
endif()
if(PRISM_FRAME_TIMING)
    target_compile_definitions(PRISM PUBLIC PRISM_FRAME_TIMING=1)
endif()
if(PRISM_DEBUG_TONE)
    target_compile_definitions(PRISM PUBLIC PRISM_DEBUG_TONE=1)
endif()

target_compile_definitions(PRISM
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# ---------------------------------------------------------------------------
# Console test runner (not part of the default build):
#   cmake --build build --target PRISM_Tests && ./build/plugins/PRISM/PRISM_Tests
# Optional sanitizers for the stress tests, e.g. configure with
#   -DPRISM_TEST_SANITIZE=thread   (or address,undefined)
# ---------------------------------------------------------------------------
add_executable(PRISM_Tests EXCLUDE_FROM_ALL
    Source/Tests/RunTests.cpp
    ${PRISM_CORE_SOURCES}
)
target_include_directories(PRISM_Tests PRIVATE Source)
target_compile_features(PRISM_Tests PRIVATE cxx_std_20)
target_link_libraries(PRISM_Tests
    PRIVATE
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
        juce::juce_gui_extra        # editor TU (WebBrowserComponent) is part of the core sources
        PRISM_UIResources
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(PRISM_Tests
    PRIVATE
        PRISM_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Source/Tests/fixtures"
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1
)
if(WIN32)
    # PRISM_Tests is a plain add_executable, so juce_add_plugin's NEEDS_WEBVIEW2
    # handling never applies to it; with the root-level
    # JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1, juce_gui_extra calls
    # CreateCoreWebView2EnvironmentWithOptions directly and needs
    # WebView2LoaderStatic.lib at link time (LNK2019 otherwise).
    if(NOT ("${JUCE_CMAKE_UTILS_DIR}" IN_LIST CMAKE_MODULE_PATH))
        list(APPEND CMAKE_MODULE_PATH "${JUCE_CMAKE_UTILS_DIR}")
    endif()
    find_package(WebView2 REQUIRED)
    target_link_libraries(PRISM_Tests PRIVATE juce::juce_webview2)
endif()
if(PRISM_TEST_SANITIZE)
    target_compile_options(PRISM_Tests PRIVATE -fsanitize=${PRISM_TEST_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(PRISM_Tests PRIVATE -fsanitize=${PRISM_TEST_SANITIZE})
endif()

# ---------------------------------------------------------------------------
# WebView UI resources - enabled in Stage 3 (GUI);
# Source/ui/public/index.html exists (copied from .ideas/mockups/v8-ui.html).
# Produces BinaryData::index_html / BinaryData::index_htmlSize.
# ---------------------------------------------------------------------------
juce_add_binary_data(PRISM_UIResources
    SOURCES
        Source/ui/public/index.html
)
target_link_libraries(PRISM PRIVATE PRISM_UIResources)
set_property(TARGET PRISM_UIResources APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/public/index.html")
