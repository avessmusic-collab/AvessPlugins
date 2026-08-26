#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

/**
 * Transitionist WebView-based Plugin Editor
 *
 * This template demonstrates the CORRECT member declaration order for WebView plugins.
 * CRITICAL: Member order prevents 90% of release build crashes.
 *
 * Destruction order (reverse of declaration):
 * 1. Attachments destroyed FIRST (stop using relays and WebView)
 * 2. WebView destroyed SECOND (safe, attachments are gone)
 * 3. Relays destroyed LAST (safe, nothing using them)
 *
 * WRONG order (attachments before WebView) causes:
 * - Destructor tries to call evaluateJavascript() on destroyed WebView
 * - Undefined behavior in release builds (optimization breaks assumptions)
 * - CRASH only in release builds (debug builds hide the bug)
 *
 * ⚠️ NOTE ON "sweep" (BIPOLAR PARAMETER):
 * "sweep" is bound with a WebSliderRelay exactly like "throw" and "space" -
 * WebSliderRelay/WebSliderParameterAttachment do not care whether the
 * underlying APVTS parameter's NormalisableRange is unipolar (0-100) or
 * bipolar (-100 to +100). Everything on the JUCE/relay/attachment side of
 * the bridge is identical for all three knobs; the ONLY bipolar-specific
 * work happens in APVTS parameter creation (Stage 2/Shell must declare
 * "sweep" with juce::NormalisableRange<float>(-100.0f, 100.0f, ...) and a
 * default of 0.0f) and in the HTML/JS arc-fill + LCD sign formatting
 * (already handled in v4-ui.html's bipolar renderer). See
 * plugins/Transitionist/.ideas/parameter-spec.md for the exact
 * NormalisableRange declaration required for "sweep".
 */

class TransitionistAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    /**
     * Constructor
     * @param p Reference to the audio processor
     */
    TransitionistAudioProcessorEditor(TransitionistAudioProcessor& p);

    /**
     * Destructor
     * Members destroyed in reverse order of declaration.
     * This is why member order matters!
     */
    ~TransitionistAudioProcessorEditor() override;

    // AudioProcessorEditor overrides
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    /**
     * Resource provider (JUCE 8 required pattern)
     * Maps URLs to embedded binary data.
     *
     * @param url Requested resource URL (e.g., "/", "/js/juce/index.js")
     * @return Resource data and MIME type, or std::nullopt for 404
     */
    std::optional<juce::WebBrowserComponent::Resource> getResource(
        const juce::String& url
    );

    // Reference to audio processor
    TransitionistAudioProcessor& audioProcessor;

    // ========================================================================
    // ⚠️ CRITICAL MEMBER DECLARATION ORDER ⚠️
    //
    // Order: Relays → WebView → Attachments
    //
    // Why: Members are destroyed in REVERSE order of declaration.
    // - Attachments must be destroyed BEFORE WebView (they call evaluateJavascript)
    // - WebView must be destroyed BEFORE Relays (it holds references via Options)
    //
    // DO NOT REORDER without understanding destructor sequence!
    // ========================================================================

    // ------------------------------------------------------------------------
    // 1️⃣ RELAYS FIRST (created first, destroyed last)
    // ------------------------------------------------------------------------
    //
    // Relays bridge C++ parameters to JavaScript state objects.
    // They have no dependencies, so they're declared first.
    //
    // Total: 3 parameters (all sliders, left-to-right signal-flow order)
    //
    // Parameter mapping:
    // - throw → WebSliderRelay (0-100%, dry/wet + delay feedback + reverb decay/size)
    // - space → WebSliderRelay (0-100%, linked delay-time/reverb-size "room" macro)
    // - sweep → WebSliderRelay (-100% to +100%, BIPOLAR - bipolar ladder filter,
    //           center detent 0.0 = fully open; see class-level note above)
    //
    std::unique_ptr<juce::WebSliderRelay> throwRelay;
    std::unique_ptr<juce::WebSliderRelay> spaceRelay;
    std::unique_ptr<juce::WebSliderRelay> sweepRelay;

    // ------------------------------------------------------------------------
    // 2️⃣ WEBVIEW SECOND (created after relays, destroyed before relays)
    // ------------------------------------------------------------------------
    //
    // WebBrowserComponent is the HTML rendering engine.
    // It depends on relays (registered via withOptionsFrom).
    //
    // Must be destroyed AFTER attachments (they call evaluateJavascript).
    // Must be destroyed BEFORE relays (holds references to them).
    //
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // ------------------------------------------------------------------------
    // 3️⃣ PARAMETER ATTACHMENTS LAST (created last, destroyed first)
    // ------------------------------------------------------------------------
    //
    // Attachments synchronize APVTS parameters with relay state.
    // They depend on BOTH the relay AND the WebView.
    //
    // MUST be declared AFTER WebView to ensure correct destruction order.
    //
    std::unique_ptr<juce::WebSliderParameterAttachment> throwAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> spaceAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> sweepAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransitionistAudioProcessorEditor)
};
