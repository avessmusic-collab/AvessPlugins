cmake_minimum_required(VERSION 3.15)

# DROP - Musical Transition Engine (BUILD -> TENSION -> VACUUM -> IMPACT -> RECOVERY).
# Audio effect: stereo in -> stereo out, plus a single configurable MIDI trigger
# note (architecture.md "MIDI Routing") - NEEDS_MIDI_INPUT without IS_SYNTH
# (juce8-critical-patterns.md #22 applies to instruments only).
# JUCE is added once at the repo-root CMakeLists.txt; this file is auto-globbed.
set(DROP_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND DROP_FORMATS AU)
endif()

juce_add_plugin(DROP
    COMPANY_NAME "PluginFreedom"
    PLUGIN_MANUFACTURER_CODE Plgf
    PLUGIN_CODE Drop
    FORMATS ${DROP_FORMATS}
    PRODUCT_NAME "DROP"
    NEEDS_MIDI_INPUT TRUE
    NEEDS_WEB_BROWSER TRUE
)

# Real translation units shared by the plugin and the console test runner
# (mirrors plugins/KICKR/CMakeLists.txt's KICKR_CORE_SOURCES convention).
set(DROP_CORE_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    # DSP - Stage 3 DSP Phase 3.1 (Timing & Control Skeleton)
    Source/DSP/MusicalClock.cpp
    Source/DSP/TransitionEngine.cpp
    Source/DSP/DropStateMachine.cpp
    # DSP - Stage 3 DSP Phase 3.2 (Core DSP Modules)
    Source/DSP/FilterModule.cpp
    Source/DSP/WidthModule.cpp
    Source/DSP/GateModule.cpp
    Source/DSP/DelayModule.cpp
    Source/DSP/SaturationModule.cpp
    Source/DSP/ReverbModule.cpp
    Source/DSP/IntensityBudget.cpp
    # DSP - Stage 3 DSP Phase 3.3 (Creative DSP Modules)
    Source/DSP/PitchModule.cpp
    Source/DSP/StutterModule.cpp
    Source/DSP/DigitalModule.cpp
    Source/DSP/VacuumModule.cpp
    Source/DSP/NoiseModule.cpp
    Source/Core/DropRandomizer.cpp
    # DSP - Stage 3 DSP Phase 3.4 (ImpactEngine)
    Source/DSP/ImpactEngine.cpp
    # DSP - Stage 3 DSP Phase 3.5 (IntensityBudget/DropStyle/AudioAnalyzer/Presets - FINAL DSP phase)
    Source/DSP/AudioAnalyzer.cpp
    Source/Core/DropStyle.cpp
    Source/Core/DropPreset.cpp
)

target_sources(DROP PRIVATE ${DROP_CORE_SOURCES})

# Include paths
target_include_directories(DROP
    PRIVATE
        Source
)

# Required JUCE modules
target_link_libraries(DROP
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
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Generate JuceHeader.h (JUCE 8 requirement - MUST come after
# target_link_libraries(), Pattern #1)
juce_generate_juce_header(DROP)

# Compile definitions
target_compile_definitions(DROP
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# ----------------------------------------------------------------------------
# WebView UI Resources - Stage 3 GUI Phase 4.1: real WebView UI wired from
# the finalized mockup v10 ("Light Neumorphic / White Marble", see
# plugins/DROP/.ideas/mockups/v10-CMakeLists.txt and
# v10-integration-checklist.md). Mirrors plugins/Transitionist/CMakeLists.txt's
# convention exactly (juce::juce_gui_extra already linked above).
# ----------------------------------------------------------------------------
juce_add_binary_data(DROP_UIResources
    SOURCES
        Source/ui/public/index.html
        Source/ui/public/js/juce/index.js
        Source/ui/public/js/juce/check_native_interop.js
        Source/ui/public/v10-marble2-texture-hd.jpg
)

target_link_libraries(DROP
    PRIVATE
        DROP_UIResources
)

target_compile_definitions(DROP
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
)

# ---------------------------------------------------------------------------
# Console test runner - Stage 3 DSP unit-test checkpoints, one block of
# checks per DSP phase (not part of the default build; run:
#   cmake --build build --target DROP_Tests
#   ./build/plugins/DROP/DROP_Tests
# Mirrors plugins/KICKR/CMakeLists.txt's KICKR_Tests convention, with one
# DROP-specific addition (Stage 3 GUI Phase 4.1): ${DROP_CORE_SOURCES}
# includes Source/PluginEditor.cpp, and PluginProcessor.cpp's createEditor()
# references the DROPAudioProcessorEditor constructor directly, so this
# target must link everything PluginEditor.cpp itself needs (juce_gui_extra,
# BinaryData from DROP_UIResources, JUCE_WEB_BROWSER=1) even though the test
# runner never actually opens an editor - otherwise the link step fails on
# an undefined WebBrowserComponent/BinaryData symbol. RunTests.cpp itself
# only includes PluginProcessor.h (see its #include block), never
# PluginEditor.h, so no GUI code ever executes in the test binary.
# ---------------------------------------------------------------------------
add_executable(DROP_Tests EXCLUDE_FROM_ALL
    Source/Tests/RunTests.cpp
    ${DROP_CORE_SOURCES}
)
target_include_directories(DROP_Tests PRIVATE Source)
target_link_libraries(DROP_Tests
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
        juce::juce_gui_extra
        DROP_UIResources
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(DROP_Tests
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
)
