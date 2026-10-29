#pragma once

#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

// Stage 1 placeholder native editor. The real GUI (TensionKnob, Visualizer,
// CustomLookAndFeel) arrives in GUI phases 4.1-4.3.
class TENSIONAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit TENSIONAudioProcessorEditor (TENSIONAudioProcessor&);
    ~TENSIONAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    TENSIONAudioProcessor& processorRef;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TENSIONAudioProcessorEditor)
};
