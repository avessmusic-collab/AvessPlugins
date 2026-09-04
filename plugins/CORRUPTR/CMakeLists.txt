cmake_minimum_required(VERSION 3.15)

# Plugin formats: VST3, AU, Standalone
juce_add_plugin(CORRUPTR
    COMPANY_NAME "VST Factory"
    PLUGIN_MANUFACTURER_CODE Vstf
    PLUGIN_CODE Corr
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "CORRUPTR"
    NEEDS_WEB_BROWSER TRUE
)

# Source files
target_sources(CORRUPTR
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

# Include paths
target_include_directories(CORRUPTR
    PRIVATE
        Source
)

# Required JUCE modules
target_link_libraries(CORRUPTR
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

# Generate JuceHeader.h (JUCE 8 requirement)
juce_generate_juce_header(CORRUPTR)

# WebView UI Resources
juce_add_binary_data(CORRUPTR_UIResources
    SOURCES
        Source/ui/public/index.html
        Source/ui/public/js/juce/index.js
        Source/ui/public/js/juce/check_native_interop.js
)

target_link_libraries(CORRUPTR
    PRIVATE
        CORRUPTR_UIResources
        juce::juce_gui_extra
)

# Compile definitions
target_compile_definitions(CORRUPTR
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# WebView support
target_compile_definitions(CORRUPTR
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
)
