# CHOPLINE — tempo-synced vocal chop / warp / glitch instrument.
# Self-contained CMake project (NOT part of the monorepo root build).
#
#   Configure:  cmake -B build -DCMAKE_BUILD_TYPE=Release        (from this dir)
#   Build:      cmake --build build --config Release --parallel
#   Tests:      ./build/CHOPLINE_Tests_artefacts/Release/CHOPLINE_Tests
#
# JUCE 9 is pinned by exact tag via FetchContent (spec §2) — the system JUCE 8
# at /Applications/JUCE is NOT used. Catch2 v3 likewise pinned (spec §2, §12).

cmake_minimum_required(VERSION 3.25)

project(CHOPLINE VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Dependencies — pinned by exact tag, never a branch (spec §2).
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
# COMPANY_NAME / PLUGIN_MANUFACTURER_CODE follow the house convention
# (neutral placeholder, de-branded 2026-09-01 — see KICKR): the user supplies
# a real company name/code before public release.
juce_add_plugin(CHOPLINE
    COMPANY_NAME "CHOPLINE"
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Chpl
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "CHOPLINE"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

# Engine/state sources are shared with the console test runner and must build
# with no GUI translation units of ours (spec §3). UI + processor/editor are
# plugin-only.
set(CHOPLINE_ENGINE_SOURCES
    src/engine/Engine.cpp
    src/state/Parameters.cpp
)

set(CHOPLINE_PLUGIN_SOURCES
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/ui/LookAndFeel.cpp
)

target_sources(CHOPLINE PRIVATE
    ${CHOPLINE_ENGINE_SOURCES}
    ${CHOPLINE_PLUGIN_SOURCES}
)

target_include_directories(CHOPLINE PRIVATE src)

target_link_libraries(CHOPLINE
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

target_compile_definitions(CHOPLINE
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

# Warnings-as-errors on OUR sources only (spec §12 "warnings as errors").
# Applied per-source so JUCE module translation units (compiled into the same
# targets by JUCE's CMake) are not affected.
if(MSVC)
    set(CHOPLINE_WERROR_FLAG "/WX")
else()
    set(CHOPLINE_WERROR_FLAG "-Werror")
endif()
set_source_files_properties(
    ${CHOPLINE_ENGINE_SOURCES} ${CHOPLINE_PLUGIN_SOURCES}
    PROPERTIES COMPILE_OPTIONS "${CHOPLINE_WERROR_FLAG}"
)

# ---------------------------------------------------------------------------
# Tests — Catch2 v3 console runner, engine/state sources only (no UI, no
# editor, no plugin wrapper). JUCE's module system still links core modules;
# juce_audio_processors transitively pulls juce_gui_extra by module
# dependency, but none of OUR GUI code is compiled in (spec §3).
# ---------------------------------------------------------------------------
juce_add_console_app(CHOPLINE_Tests PRODUCT_NAME "CHOPLINE_Tests")

target_sources(CHOPLINE_Tests PRIVATE
    tests/SanityTests.cpp
    tests/ParameterLayoutTests.cpp
    ${CHOPLINE_ENGINE_SOURCES}
)

target_include_directories(CHOPLINE_Tests PRIVATE src)

target_link_libraries(CHOPLINE_Tests
    PRIVATE
        Catch2::Catch2WithMain
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_compile_definitions(CHOPLINE_Tests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

set_source_files_properties(
    tests/SanityTests.cpp tests/ParameterLayoutTests.cpp
    PROPERTIES COMPILE_OPTIONS "${CHOPLINE_WERROR_FLAG}"
)

enable_testing()
add_test(NAME CHOPLINE_Tests COMMAND CHOPLINE_Tests)
