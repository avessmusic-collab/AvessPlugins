cmake_minimum_required(VERSION 3.15)

# KICKR — dedicated algorithmic kick-design instrument.
# Synth (IS_SYNTH), MIDI-triggered, output-only stereo bus, native JUCE UI (no WebView).
# JUCE is added once at the repo-root CMakeLists.txt; this file is auto-globbed.

juce_add_plugin(KICKR
    COMPANY_NAME "PluginFreedom"
    PLUGIN_MANUFACTURER_CODE Plgf
    PLUGIN_CODE Kckr
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "KICKR"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    IS_MIDI_EFFECT FALSE
)

# Real translation units shared by the plugin and the console test runner.
# (DSP/UI/Preset stub headers are compiled once they are #included.)
set(KICKR_CORE_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Utilities/DSPUtils.cpp
    Source/Tests/OfflineRender.cpp
    # DSP — Stage 2 Phase 2.1 (OS-region shell + MIDI-triggered basic kick)
    Source/DSP/OversamplingProcessor.cpp
    Source/DSP/BodyOscillator.cpp
    Source/DSP/AmplitudeEnvelope.cpp
    Source/DSP/KickVoice.cpp
    Source/DSP/KickEngine.cpp
    # DSP — Stage 2 Phase 2.3 (transient shaper + click-free 2-voice retrigger)
    Source/DSP/TransientShaper.cpp
    # DSP — Stage 2 Phase 2.4 (synthesised click generator)
    Source/DSP/ClickGenerator.cpp
    # DSP — Stage 2 Phase 2.5 (independent mono sub oscillator)
    Source/DSP/SubOscillator.cpp
    # DSP — Stage 2 Phase 2.6 (dedicated LF tail generator)
    Source/DSP/TailGenerator.cpp
    # DSP — Stage 2 Phase 2.7 (optional white/pink/filtered noise layer)
    Source/DSP/NoiseGenerator.cpp
    # DSP — Stage 2 Phase 2.7b (sample player + managed drag-drop library)
    Source/DSP/SamplePlayer.cpp
    Source/Sampling/SampleLibrary.cpp
    # DSP — Stage 2 Phase 2.8 (master morphing distortion — 7-curve character morph)
    Source/DSP/Waveshaper.cpp
    # DSP — Stage 2 Phase 2.9 (tone / stereo width + mono crossover / output gain / limiter / mix)
    Source/DSP/OutputStage.cpp
    # GUI — Stage 3 Phase 3.1 (native LookAndFeel + compound Knob/Toggle + full editor layout)
    Source/UI/KickrLookAndFeel.cpp
    Source/UI/KickrKnob.cpp
    # GUI — Stage 3 Phase 3.2 (real-time waveform + spectrum analyzers)
    Source/DSP/Analyzer.cpp
    Source/UI/WaveformDisplay.cpp
    Source/UI/SpectrumDisplay.cpp
)

target_sources(KICKR PRIVATE ${KICKR_CORE_SOURCES})

# Include paths — everything under Source/ is reachable by "Subdir/Header.h".
target_include_directories(KICKR
    PRIVATE
        Source
)

# Required JUCE modules
target_link_libraries(KICKR
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
juce_generate_juce_header(KICKR)

# Compile definitions
target_compile_definitions(KICKR
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

# ---------------------------------------------------------------------------
# Console test runner — Stage 2 offline-render checkpoints (not part of the
# default build; run:  cmake --build build --target KICKR_Tests)
# ---------------------------------------------------------------------------
add_executable(KICKR_Tests EXCLUDE_FROM_ALL
    Source/Tests/RunTests.cpp
    ${KICKR_CORE_SOURCES}
)
target_include_directories(KICKR_Tests PRIVATE Source)
target_link_libraries(KICKR_Tests
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
target_compile_definitions(KICKR_Tests
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
)

# ---------------------------------------------------------------------------
# Factory preset BinaryData — added in Stage 3 (Phase 3.3 — Preset system).
# Source/Presets/ will hold *.kickrpreset (or generated *.xml) factory patches.
#
# juce_add_binary_data(KICKR_Presets
#     SOURCES
#         Source/Presets/Factory/Default.kickrpreset
#         ...
# )
# target_link_libraries(KICKR PRIVATE KICKR_Presets)
# ---------------------------------------------------------------------------
