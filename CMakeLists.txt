cmake_minimum_required(VERSION 3.22)

# Set deployment target before project
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS deployment version")

project(JUCEPlugins VERSION 1.0.0)

# Add JUCE once at root: local install when present (macOS dev machine),
# otherwise fetch pinned JUCE 8 (CI / other machines).
if(EXISTS "/Applications/JUCE/CMakeLists.txt")
    add_subdirectory(/Applications/JUCE JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15 # match the local /Applications/JUCE version exactly
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# Auto-discover plugins. -DONLY_PLUGIN=<Name> restricts configuration to one
# plugin (used by CI so one plugin's build never depends on the others).
file(GLOB PLUGIN_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/*")
foreach(PLUGIN_DIR ${PLUGIN_DIRS})
    if(IS_DIRECTORY ${PLUGIN_DIR} AND EXISTS "${PLUGIN_DIR}/CMakeLists.txt")
        get_filename_component(PLUGIN_NAME ${PLUGIN_DIR} NAME)
        if(DEFINED ONLY_PLUGIN AND NOT PLUGIN_NAME STREQUAL "${ONLY_PLUGIN}")
            continue()
        endif()
        add_subdirectory(${PLUGIN_DIR})
    endif()
endforeach()
