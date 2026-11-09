cmake_minimum_required(VERSION 3.15)

# SPECTR - bit-transparent multi-source analyzer (audio effect: stereo in/out +
# optional sidechain input; no MIDI). Adapted from
# .ideas/mockups/v8-CMakeLists.txt. JUCE is added once at the repo-root
# CMakeLists.txt; this file is auto-discovered (no project()/add_subdirectory(JUCE)).
set(SPECTR_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SPECTR_FORMATS AU)
endif()

juce_add_plugin(SPECTR
    COMPANY_NAME "PluginFreedom"
    PLUGIN_MANUFACTURER_CODE Plgf
    PLUGIN_CODE Sptr
    FORMATS ${SPECTR_FORMATS}
    PRODUCT_NAME "SPECTR"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    NEEDS_WEB_BROWSER TRUE      # Pattern #9 (VST3 WebView); Stage 3 uses it
    NEEDS_WEBVIEW2 TRUE         # Windows WebView2 backend; ignored elsewhere
)

# Translation units shared by the plugin and the console test runner.
set(SPECTR_CORE_SOURCES
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

target_sources(SPECTR PRIVATE ${SPECTR_CORE_SOURCES})

target_include_directories(SPECTR
    PRIVATE
        Source
)

target_compile_features(SPECTR PRIVATE cxx_std_20)

target_link_libraries(SPECTR
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
juce_generate_juce_header(SPECTR)

# Debug-only switches (OFF by default): -DSPECTR_FRAME_TIMING=ON logs pushAnalysisFrame timing to stderr,
# -DSPECTR_DEBUG_TONE=ON analyses a synthetic 1 kHz / -12 dBFS stereo sine instead of the input.
option(SPECTR_FRAME_TIMING "SPECTR: log editor frame-push timing" OFF)
option(SPECTR_DEBUG_TONE "SPECTR: analyse a synthetic test tone (debug only)" OFF)
option(SPECTR_DEBUG_CONTROL "SPECTR: execute JS from $SPECTR_DEBUG_JS_FILE (debug only)" OFF)
if(SPECTR_DEBUG_CONTROL)
    target_compile_definitions(SPECTR PUBLIC SPECTR_DEBUG_CONTROL=1)
endif()
if(SPECTR_FRAME_TIMING)
    target_compile_definitions(SPECTR PUBLIC SPECTR_FRAME_TIMING=1)
endif()
if(SPECTR_DEBUG_TONE)
    target_compile_definitions(SPECTR PUBLIC SPECTR_DEBUG_TONE=1)
endif()

target_compile_definitions(SPECTR
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# ---------------------------------------------------------------------------
# Console test runner (not part of the default build):
#   cmake --build build --target SPECTR_Tests && ./build/plugins/SPECTR/SPECTR_Tests
# Optional sanitizers for the stress tests, e.g. configure with
#   -DSPECTR_TEST_SANITIZE=thread   (or address,undefined)
# ---------------------------------------------------------------------------
add_executable(SPECTR_Tests EXCLUDE_FROM_ALL
    Source/Tests/RunTests.cpp
    ${SPECTR_CORE_SOURCES}
)
target_include_directories(SPECTR_Tests PRIVATE Source)
target_compile_features(SPECTR_Tests PRIVATE cxx_std_20)
target_link_libraries(SPECTR_Tests
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
        SPECTR_UIResources
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(SPECTR_Tests
    PRIVATE
        SPECTR_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Source/Tests/fixtures"
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1
)
if(WIN32)
    # SPECTR_Tests is a plain add_executable, so juce_add_plugin's NEEDS_WEBVIEW2
    # handling never applies to it; with the root-level
    # JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1, juce_gui_extra calls
    # CreateCoreWebView2EnvironmentWithOptions directly and needs
    # WebView2LoaderStatic.lib at link time (LNK2019 otherwise).
    if(NOT ("${JUCE_CMAKE_UTILS_DIR}" IN_LIST CMAKE_MODULE_PATH))
        list(APPEND CMAKE_MODULE_PATH "${JUCE_CMAKE_UTILS_DIR}")
    endif()
    find_package(WebView2 REQUIRED)
    target_link_libraries(SPECTR_Tests PRIVATE juce::juce_webview2)
    # The analysis/stress tests use large stack buffers (sanitizer builds
    # already need a 64MB stack); Windows' default 1MB overflows mid-suite
    # with STATUS_STACK_OVERFLOW (0xC00000FD).
    target_link_options(SPECTR_Tests PRIVATE /STACK:0x4000000)
endif()
if(SPECTR_TEST_SANITIZE)
    target_compile_options(SPECTR_Tests PRIVATE -fsanitize=${SPECTR_TEST_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(SPECTR_Tests PRIVATE -fsanitize=${SPECTR_TEST_SANITIZE})
endif()

# ---------------------------------------------------------------------------
# WebView UI resources - enabled in Stage 3 (GUI);
# Source/ui/public/index.html exists (copied from .ideas/mockups/v8-ui.html).
# Produces BinaryData::index_html / BinaryData::index_htmlSize.
# ---------------------------------------------------------------------------
juce_add_binary_data(SPECTR_UIResources
    SOURCES
        Source/ui/public/index.html
)
target_link_libraries(SPECTR PRIVATE SPECTR_UIResources)
set_property(TARGET SPECTR_UIResources APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/ui/public/index.html")
