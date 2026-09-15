#pragma once
#include "PluginProcessor.h"

/**
 * Stage 1 placeholder editor. Full WebView UI (finalized mockup v10 "Light
 * Neumorphic / White Marble") is wired up in Stage 3 GUI Phases 4.1-4.3, per
 * plugins/DROP/.ideas/mockups/v10-PluginEditor.h/.cpp and
 * v10-integration-checklist.md. Default/min/max sizes below already match
 * the finalized mockup's base 1000x560, resizable (min 800x450, max
 * 1400x790) so this placeholder can be swapped for the real UI without a
 * host-visible size jump.
 */
class DROPAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit DROPAudioProcessorEditor(DROPAudioProcessor&);
    ~DROPAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    DROPAudioProcessor& processorRef;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DROPAudioProcessorEditor)
};
