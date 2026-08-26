#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

/**
 * Transitionist WebView-based Plugin Editor - v3 (2026-08-27: removed
 * input/output gain sliders + meters, replaced with a single small VOLUME
 * knob, last in chain). Built from .ideas/mockups/v5-ui.html.
 *
 * Member declaration order: Relays -> WebView -> Attachments (destroyed in
 * reverse). See v1's PluginEditor.h for the full rationale (unchanged).
 *
 * 6 parameters: transition, reverb, delay, delaySync (CHOICE - uses
 * WebComboBoxRelay/WebComboBoxParameterAttachment), dryWet, volume. No
 * Timer/level-meter mechanism in this revision (removed along with the
 * input/output gain sliders they were paired with).
 */
class TransitionistAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit TransitionistAudioProcessorEditor(TransitionistAudioProcessor& p);
    ~TransitionistAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    TransitionistAudioProcessor& processorRef;

    // ------------------------------------------------------------------
    // 1) RELAYS FIRST
    // ------------------------------------------------------------------
    std::unique_ptr<juce::WebSliderRelay> transitionRelay;
    std::unique_ptr<juce::WebSliderRelay> reverbRelay;
    std::unique_ptr<juce::WebSliderRelay> delayRelay;
    std::unique_ptr<juce::WebComboBoxRelay> delaySyncRelay;
    std::unique_ptr<juce::WebSliderRelay> dryWetRelay;
    std::unique_ptr<juce::WebSliderRelay> volumeRelay;

    // ------------------------------------------------------------------
    // 2) WEBVIEW SECOND
    // ------------------------------------------------------------------
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // ------------------------------------------------------------------
    // 3) ATTACHMENTS LAST
    // ------------------------------------------------------------------
    std::unique_ptr<juce::WebSliderParameterAttachment> transitionAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> reverbAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> delayAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> delaySyncAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> dryWetAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> volumeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransitionistAudioProcessorEditor)
};
