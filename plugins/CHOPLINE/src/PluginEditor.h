// CHOPLINE — PluginEditor
// Phase 0: empty native-JUCE editor. Plain light background (wireframe-light
// direction, creative brief v17) with a phase label. Resizable 1100x720,
// 75%–200%, fixed aspect ratio (spec §9).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "ui/LookAndFeel.h"

namespace chopline
{

class ChoplineAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit ChoplineAudioProcessorEditor (ChoplineAudioProcessor&);
    ~ChoplineAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int defaultWidth = 1100;
    static constexpr int defaultHeight = 720;

    ChoplineAudioProcessor& processorRef;
    ui::ChoplineLookAndFeel lookAndFeel;
    juce::Label phaseLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoplineAudioProcessorEditor)
};

} // namespace chopline
