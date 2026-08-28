#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace kd2
{
    /**
        Stage 1 stub. Implemented in Stage 3 (plan.md Phase 3.3).

        Factory presets baked in via juce_add_binary_data (see CMakeLists.txt — added
        in Phase 3.3). User presets saved to / loaded from:
            ~/Library/Audio/Presets/PluginFreedom/KickDesigner2/
        Apply by replacing the APVTS `state` tree (message thread only — all preset
        I/O is off the audio thread). UndoManager-backed. 17 factory patches:
        Clean, House, Techno, Hard Techno, Hardstyle, Hardcore, Industrial, Sub Heavy,
        Short, Long, Distorted, Clicky, Punchy, Warehouse, EDM, Trap, Cinematic.
    */
    class PresetManager
    {
    public:
        explicit PresetManager (juce::AudioProcessorValueTreeState& stateToManage)
            : apvts (stateToManage) {}

    private:
        juce::AudioProcessorValueTreeState& apvts;
    };
}
