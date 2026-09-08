#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

/**
 * CORRUPTR WebView-based Plugin Editor
 *
 * UI Mockup: v5 (1200x1650 total scrollable content, 1200x800 visible viewport,
 * fixed/non-resizable, internal vertical scroll - see v5-ui.yaml `window` block)
 * Parameters: 56 total (26 Float / 18 Bool / 12 Choice)
 * Full contract: plugins/CORRUPTR/.ideas/parameter-spec.md (locked)
 *
 * CRITICAL: Member declaration order prevents release build crashes.
 * Order: Relays -> WebView -> Attachments
 *
 * Destruction order (reverse of declaration):
 * 1. Attachments destroyed FIRST (stop using relays and WebView)
 * 2. WebView destroyed SECOND (safe, attachments are gone)
 * 3. Relays destroyed LAST (safe, nothing using them)
 *
 * NOTE ON DUAL-BINDING PAIRS: filterCutoff and glitchProbability are each
 * bound to TWO controls in v5-ui.html (a base knob + an ADV-revealed fine
 * knob) but get exactly ONE relay + ONE attachment each below - the JUCE
 * frontend library's getSliderState()/getComboBoxState()/getToggleState()
 * cache state objects per parameter name, so both DOM controls share the
 * same underlying relay automatically. Do NOT create a second relay for
 * these parameters.
 *
 * NOTE ON ENABLE vs BYPASS SEMANTICS: sequencerEnabled/modMatrixEnabled use
 * true=ON polarity (opposite of the 5 graphBypass* toggles, which use
 * true=BYPASSED). This is a pure default-value/DSP-usage distinction - the
 * relay/attachment plumbing for a WebToggleButtonRelay is IDENTICAL
 * regardless of polarity.
 */

class CORRUPTRAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    CORRUPTRAudioProcessorEditor(CORRUPTRAudioProcessor& p);
    ~CORRUPTRAudioProcessorEditor() override;

    // AudioProcessorEditor overrides
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    /**
     * Resource provider (JUCE 8 required pattern)
     * Maps URLs to embedded binary data from Source/ui/public/
     *
     * @param url Requested resource URL (e.g., "/", "/index.html", "/js/juce/index.js")
     * @return Resource data and MIME type, or std::nullopt for 404
     */
    std::optional<juce::WebBrowserComponent::Resource> getResource(
        const juce::String& url
    );

    // Reference to audio processor
    CORRUPTRAudioProcessor& audioProcessor;

    // ========================================================================
    // ⚠️ CRITICAL MEMBER DECLARATION ORDER ⚠️
    //
    // Order: Relays -> WebView -> Attachments
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
    // 56 parameters, module-grouped (matches v5-ui.yaml/parameter-spec.md order):
    //
    // --- Top Bar / Global ---
    std::unique_ptr<juce::WebComboBoxRelay> qualityModeRelay;

    // --- Distortion ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassSaturationRelay;
    std::unique_ptr<juce::WebComboBoxRelay> distortionAlgorithmRelay;
    std::unique_ptr<juce::WebSliderRelay> driveRelay;
    std::unique_ptr<juce::WebSliderRelay> foldRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassWaveshaperRelay;

    // --- Bitcrush ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassBitcrushRelay;
    std::unique_ptr<juce::WebSliderRelay> bitDepthRelay;

    // --- Glitch ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassGlitchRelay;
    std::unique_ptr<juce::WebComboBoxRelay> glitchModeRelay;
    std::unique_ptr<juce::WebComboBoxRelay> glitchBufferLengthRelay;
    std::unique_ptr<juce::WebSliderRelay> glitchProbabilityRelay;

    // --- Center Master ---
    std::unique_ptr<juce::WebSliderRelay> mixRelay;
    std::unique_ptr<juce::WebComboBoxRelay> outputLimiterStyleRelay;
    std::unique_ptr<juce::WebSliderRelay> inputGainRelay;
    std::unique_ptr<juce::WebSliderRelay> outputGainRelay;

    // --- Filter & EQ ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassFilterRelay;
    std::unique_ptr<juce::WebComboBoxRelay> filterTypeRelay;
    std::unique_ptr<juce::WebSliderRelay> filterCutoffRelay;
    std::unique_ptr<juce::WebSliderRelay> filterResonanceRelay;

    // --- Feedback + Delay ---
    std::unique_ptr<juce::WebSliderRelay> feedbackAmountRelay;
    std::unique_ptr<juce::WebSliderRelay> feedbackDampingRelay;
    std::unique_ptr<juce::WebSliderRelay> microDelayTimeRelay;

    // --- Macros ---
    std::unique_ptr<juce::WebSliderRelay> macroDamageRelay;
    std::unique_ptr<juce::WebSliderRelay> macroCrushRelay;
    std::unique_ptr<juce::WebSliderRelay> macroGlitchRelay;
    std::unique_ptr<juce::WebSliderRelay> macroChaosRelay;
    std::unique_ptr<juce::WebSliderRelay> macroRhythmRelay;
    std::unique_ptr<juce::WebSliderRelay> macroMovementRelay;
    std::unique_ptr<juce::WebSliderRelay> macroWidthRelay;
    std::unique_ptr<juce::WebSliderRelay> macroMixRelay;

    // --- Performance Triggers ---
    std::unique_ptr<juce::WebToggleButtonRelay> performanceKillRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceGlitchRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceDestroyRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceFreezeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceReverseRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceStutterRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> performanceChaosRelay;

    // --- XY Performance Pad ---
    std::unique_ptr<juce::WebSliderRelay> xyPadXRelay;
    std::unique_ptr<juce::WebSliderRelay> xyPadYRelay;

    // --- Rhythmic Sequencer ---
    std::unique_ptr<juce::WebToggleButtonRelay> sequencerEnabledRelay;
    std::unique_ptr<juce::WebComboBoxRelay> sequencerRateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> sequencerStepsRelay;

    // --- Modulation Matrix (4 LFOs) ---
    std::unique_ptr<juce::WebToggleButtonRelay> modMatrixEnabledRelay;
    std::unique_ptr<juce::WebSliderRelay> lfo1RateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> lfo1ShapeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> lfo1SyncRelay;
    std::unique_ptr<juce::WebSliderRelay> lfo2RateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> lfo2ShapeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> lfo2SyncRelay;
    std::unique_ptr<juce::WebSliderRelay> lfo3RateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> lfo3ShapeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> lfo3SyncRelay;
    std::unique_ptr<juce::WebSliderRelay> lfo4RateRelay;
    std::unique_ptr<juce::WebComboBoxRelay> lfo4ShapeRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> lfo4SyncRelay;

    // ------------------------------------------------------------------------
    // 2️⃣ WEBVIEW SECOND (created after relays, destroyed before relays)
    // ------------------------------------------------------------------------
    //
    // WebBrowserComponent renders Source/ui/public/index.html
    // Registered with all 56 relays via .withOptionsFrom(*relay)
    //
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // ------------------------------------------------------------------------
    // 3️⃣ PARAMETER ATTACHMENTS LAST (created last, destroyed first)
    // ------------------------------------------------------------------------
    //
    // Synchronize APVTS parameters with WebView relay state
    //
    // --- Top Bar / Global ---
    std::unique_ptr<juce::WebComboBoxParameterAttachment> qualityModeAttachment;

    // --- Distortion ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassSaturationAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> distortionAlgorithmAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> foldAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassWaveshaperAttachment;

    // --- Bitcrush ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassBitcrushAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> bitDepthAttachment;

    // --- Glitch ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassGlitchAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> glitchModeAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> glitchBufferLengthAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> glitchProbabilityAttachment;

    // --- Center Master ---
    std::unique_ptr<juce::WebSliderParameterAttachment> mixAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> outputLimiterStyleAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> inputGainAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> outputGainAttachment;

    // --- Filter & EQ ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassFilterAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> filterTypeAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> filterCutoffAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> filterResonanceAttachment;

    // --- Feedback + Delay ---
    std::unique_ptr<juce::WebSliderParameterAttachment> feedbackAmountAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> feedbackDampingAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> microDelayTimeAttachment;

    // --- Macros ---
    std::unique_ptr<juce::WebSliderParameterAttachment> macroDamageAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroCrushAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroGlitchAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroChaosAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroRhythmAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroMovementAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroWidthAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> macroMixAttachment;

    // --- Performance Triggers ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceKillAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceGlitchAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceDestroyAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceFreezeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceReverseAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceStutterAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> performanceChaosAttachment;

    // --- XY Performance Pad ---
    std::unique_ptr<juce::WebSliderParameterAttachment> xyPadXAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> xyPadYAttachment;

    // --- Rhythmic Sequencer ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> sequencerEnabledAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> sequencerRateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> sequencerStepsAttachment;

    // --- Modulation Matrix (4 LFOs) ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> modMatrixEnabledAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> lfo1RateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> lfo1ShapeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> lfo1SyncAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> lfo2RateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> lfo2ShapeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> lfo2SyncAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> lfo3RateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> lfo3ShapeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> lfo3SyncAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> lfo4RateAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> lfo4ShapeAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> lfo4SyncAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessorEditor)
};
