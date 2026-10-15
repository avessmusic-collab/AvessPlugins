#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Stage 1 placeholder editor. Replaced in Stage 3 by the WebView editor
// (.ideas/mockups/v8-PluginEditor.{h,cpp}); class name and processor API match it.
class PRISMAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit PRISMAudioProcessorEditor(PRISMAudioProcessor&);
    ~PRISMAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    PRISMAudioProcessor& processorRef;
    juce::ComponentBoundsConstrainer constrainer;   // 55:36 aspect, min 770x504

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PRISMAudioProcessorEditor)
};
