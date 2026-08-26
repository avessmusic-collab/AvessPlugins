#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

/**
 * Transitionist WebView-based Plugin Editor - v2 (full control-surface
 * redesign, 2026-08-26). Built from .ideas/mockups/v5-ui.html.
 *
 * Member declaration order: Relays -> WebView -> Attachments (destroyed in
 * reverse). See v1's PluginEditor.h for the full rationale (unchanged).
 *
 * 7 parameters: transition, reverb, delay, delaySync (CHOICE - uses
 * WebComboBoxRelay/WebComboBoxParameterAttachment, not WebSliderRelay),
 * dryWet, inputGain, outputGain. Plus a 30Hz Timer pushing 2 live level
 * meters (inputLevelDb/outputLevelDb) to the WebView via
 * emitEventIfBrowserIsVisible - these are NOT parameters, no relay/
 * attachment involved (see PluginProcessor.h).
 */
class TransitionistAudioProcessorEditor : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    explicit TransitionistAudioProcessorEditor(TransitionistAudioProcessor& p);
    ~TransitionistAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

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
    std::unique_ptr<juce::WebSliderRelay> inputGainRelay;
    std::unique_ptr<juce::WebSliderRelay> outputGainRelay;

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
    std::unique_ptr<juce::WebSliderParameterAttachment> inputGainAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> outputGainAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransitionistAudioProcessorEditor)
};
