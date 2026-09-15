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
# WebView UI Resources - Stage 3 GUI Phase 4.1 wires this up for real, from
# the finalized mockup v10 ("Light Neumorphic / White Marble", see
# plugins/DROP/.ideas/mockups/v10-CMakeLists.txt and
# v10-integration-checklist.md). Left commented out here: Stage 1 Foundation's
# editor is a placeholder (Source/ui/public/* does not exist yet), and the
# full WebView wiring is explicitly out of scope for this stage.
# ----------------------------------------------------------------------------
# juce_add_binary_data(DROP_UIResources
#     SOURCES
#         Source/ui/public/index.html
#         Source/ui/public/js/juce/index.js
#         Source/ui/public/js/juce/check_native_interop.js
#         Source/ui/public/v10-marble2-texture-hd.jpg
# )
#
# target_link_libraries(DROP
#     PRIVATE
#         DROP_UIResources
# )
#
# target_compile_definitions(DROP
#     PUBLIC
#         JUCE_WEB_BROWSER=1
#         JUCE_USE_CURL=0
# )

# ---------------------------------------------------------------------------
# Console test runner - Stage 3 DSP unit-test checkpoints, one block of
# checks per DSP phase (not part of the default build; run:
#   cmake --build build --target DROP_Tests
#   ./build/plugins/DROP/DROP_Tests
# Mirrors plugins/KICKR/CMakeLists.txt's KICKR_Tests convention exactly.
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
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
target_compile_definitions(DROP_Tests
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
)
