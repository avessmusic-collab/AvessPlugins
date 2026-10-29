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
