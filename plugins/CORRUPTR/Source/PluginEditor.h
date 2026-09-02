#pragma once
#include "PluginProcessor.h"

class CORRUPTRAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit CORRUPTRAudioProcessorEditor(CORRUPTRAudioProcessor&);
    ~CORRUPTRAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    CORRUPTRAudioProcessor& processorRef;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessorEditor)
};
