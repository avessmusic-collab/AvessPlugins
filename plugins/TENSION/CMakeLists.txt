cmake_minimum_required(VERSION 3.15)

# TENSION - one-knob cinematic tension/riser (audio effect).
# Stereo/mono in -> same out. No MIDI. Native JUCE GUI (NO WebView):
# NEEDS_WEB_BROWSER FALSE, no BinaryData UI resources, no juce_gui_extra.
# JUCE is added once at the repo-root CMakeLists.txt; this file is auto-globbed
# (do NOT add project() or add_subdirectory(JUCE) here).
set(TENSION_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND TENSION_FORMATS AU)
endif()

juce_add_plugin(TENSION
    COMPANY_NAME "PluginFreedom"
    PLUGIN_MANUFACTURER_CODE Plgf
    PLUGIN_CODE Tnsn
    FORMATS ${TENSION_FORMATS}
    PRODUCT_NAME "TENSION"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    NEEDS_WEB_BROWSER FALSE
)

# Real translation units (later phases append DSP/UI sources here and the
# console test runner can share the list, mirroring DROP/KICKR conventions).
set(TENSION_CORE_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    # DSP - Stage 3 Phase 3.1 (engine skeleton + OutputSafety; TensionMapping.h is header-only)
    Source/DSP/TensionEngine.cpp
    Source/DSP/OutputSafety.cpp
    # DSP - Stage 3 Phase 3.2 (core series modules)
    Source/DSP/LFOEngine.cpp
    Source/DSP/FilterEngine.cpp
    Source/DSP/SaturationEngine.cpp
    Source/DSP/StereoWidth.cpp
    # DSP - Stage 3 Phase 3.3 (parallel sends and noise; AsymSmoother.h is header-only)
    Source/DSP/NoiseGenerator.cpp
    Source/DSP/ReverbEngine.cpp
    Source/DSP/DelayEngine.cpp
    # GUI - Stage 4 Phase 4.1 (native JUCE; juce_gui_basics/graphics only, already linked by the test targets)
    Source/UI/CustomLookAndFeel.cpp
    Source/UI/TensionKnob.cpp
    Source/UI/Visualizer.cpp
    Source/UI/ControlWidgets.cpp
)

target_sources(TENSION PRIVATE ${TENSION_CORE_SOURCES})

target_compile_features(TENSION PRIVATE cxx_std_20)

target_include_directories(TENSION
    PRIVATE
        Source
)

# Required JUCE modules (no juce_gui_extra: native GUI only)
target_link_libraries(TENSION
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Generate JuceHeader.h (JUCE 8 requirement - MUST come after
# target_link_libraries(), Pattern #1)
juce_generate_juce_header(TENSION)

# Compile definitions
target_compile_definitions(TENSION
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

# ---------------------------------------------------------------------------
# Console test runner (not part of the default build; run:
#   cmake --build build --target TENSION_Tests
#   ./build/plugins/TENSION/TENSION_Tests )
# ---------------------------------------------------------------------------
add_executable(TENSION_Tests EXCLUDE_FROM_ALL
    Source/Tests/RunTests.cpp
    ${TENSION_CORE_SOURCES}
)
target_compile_features(TENSION_Tests PRIVATE cxx_std_20)
target_include_directories(TENSION_Tests PRIVATE Source)
target_link_libraries(TENSION_Tests
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
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(TENSION_Tests
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
)

# ---------------------------------------------------------------------------
# Phase 3.5 metric harness (offline renders, text/CSV report; not part of the default build):
#   cmake --build build --target TENSION_Profiles
#   ./build/plugins/TENSION/TENSION_Profiles [--quick] [--comp] [--out <dir>]
# Writes logs/TENSION/metrics_<timestamp>/summary.txt + one CSV per ramp render.
# ---------------------------------------------------------------------------
add_executable(TENSION_Profiles EXCLUDE_FROM_ALL
    Source/Tests/RunProfiles.cpp
    ${TENSION_CORE_SOURCES}
)
target_compile_features(TENSION_Profiles PRIVATE cxx_std_20)
target_include_directories(TENSION_Profiles PRIVATE Source)
target_link_libraries(TENSION_Profiles
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
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(TENSION_Profiles
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
)

# ---------------------------------------------------------------------------
# Offscreen editor snapshot (dev tool for GUI review; not part of the default build):
#   cmake --build build --target TENSION_UISnapshot
#   ./build/plugins/TENSION/TENSION_UISnapshot out.png [tension%] [riseMode] [animSeconds]
# ---------------------------------------------------------------------------
add_executable(TENSION_UISnapshot EXCLUDE_FROM_ALL
    Source/Tests/RenderUI.cpp
    ${TENSION_CORE_SOURCES}
)
target_compile_features(TENSION_UISnapshot PRIVATE cxx_std_20)
target_include_directories(TENSION_UISnapshot PRIVATE Source)
target_link_libraries(TENSION_UISnapshot
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
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(TENSION_UISnapshot
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1
)
