cmake_minimum_required(VERSION 3.15)

# KickDesigner2 — dedicated algorithmic kick-design instrument.
# Synth (IS_SYNTH), MIDI-triggered, output-only stereo bus, native JUCE UI (no WebView).
# JUCE is added once at the repo-root CMakeLists.txt; this file is auto-globbed.

juce_add_plugin(KickDesigner2
    COMPANY_NAME "PluginFreedom"
    PLUGIN_MANUFACTURER_CODE Plgf
    PLUGIN_CODE Kd02
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Kick Designer 2"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    IS_MIDI_EFFECT FALSE
)

# Source files (only real translation units — DSP/UI/Preset headers below are Stage 1 stubs
# and are compiled once they are #included in Stage 2 / Stage 3).
target_sources(KickDesigner2
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Utilities/DSPUtils.cpp
        Source/Tests/OfflineRender.cpp
)

# Include paths — everything under Source/ is reachable by "Subdir/Header.h".
target_include_directories(KickDesigner2
    PRIVATE
        Source
)

# Required JUCE modules
target_link_libraries(KickDesigner2
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

# Generate JuceHeader.h (JUCE 8 requirement — Pattern #1: MUST come after target_link_libraries)
juce_generate_juce_header(KickDesigner2)

# Compile definitions
target_compile_definitions(KickDesigner2
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

# ---------------------------------------------------------------------------
# Factory preset BinaryData — added in Stage 3 (Phase 3.3 — Preset system).
# Source/Presets/ will hold *.kd2preset (or generated *.xml) factory patches.
#
# juce_add_binary_data(KickDesigner2_Presets
#     SOURCES
#         Source/Presets/Factory/Default.kd2preset
#         ...
# )
# target_link_libraries(KickDesigner2 PRIVATE KickDesigner2_Presets)
# ---------------------------------------------------------------------------
