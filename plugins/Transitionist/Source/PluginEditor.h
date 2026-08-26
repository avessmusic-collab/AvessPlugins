#pragma once
#include "PluginProcessor.h"

/**
 * Transitionist - Stage 1 (Foundation + Shell) placeholder editor.
 *
 * This is a minimal, non-WebView editor that compiles and opens so the
 * plugin can be verified end-to-end (build + load in DAW/standalone) before
 * Stage 3 (GUI) replaces this with the real WebView editor built from the
 * finalized v4 UI mockup (see plugins/Transitionist/.ideas/mockups/v4-*).
 */
class TransitionistAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit TransitionistAudioProcessorEditor(TransitionistAudioProcessor&);
    ~TransitionistAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    TransitionistAudioProcessor& processorRef;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransitionistAudioProcessorEditor)
};
