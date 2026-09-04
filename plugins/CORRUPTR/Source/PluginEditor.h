#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

/**
 * CORRUPTR WebView-based Plugin Editor
 *
 * UI Mockup: v5 (1200x1650 total scrollable content, 1200x800 visible viewport,
 * fixed/non-resizable, internal vertical scroll - see v5-ui.yaml `window` block)
 * Full contract: plugins/CORRUPTR/.ideas/parameter-spec.md (locked, 94 total parameters)
 *
 * ============================================================================
 * STAGE 3 PHASED GUI ROLLOUT (plan.md "Stage 3: GUI Phases", 5.1-5.6)
 * ============================================================================
 * This editor loads the FULL v5-ui.html production mockup (all 12
 * modules/strips render visually, per plan.md's phasing intent that later
 * phases only need to WIRE bindings, not redesign markup), but Phase 5.1
 * binds ONLY the Distortion / Filter & EQ / Center Master (Output) / Top Bar
 * (qualityMode) parameter set below (14 total) - the "core chain" per
 * plan.md's Phase 5.1 goal.
 *
 * All other HTML controls (Bitcrush, Glitch, Feedback+Delay, Macros,
 * Performance Triggers, XY Pad, Sequencer, Mod Matrix) remain VISUALLY
 * present and locally interactive (the mockup's generic per-control-type
 * bindXxx() JS helpers call Juce.getSliderState()/getComboBoxState()/
 * getToggleState() for EVERY element carrying a data-param attribute,
 * regardless of phase) but are INERT with respect to the real APVTS/audio
 * engine this phase, because no C++ relay is registered for their parameter
 * IDs below - JUCE's WebView bridge silently falls back to local-only state
 * for any parameter name with no matching relay (console may log
 * "unknown to the backend" warnings for those IDs; this is expected and
 * matches v5-integration-checklist.md's own documented troubleshooting
 * note for this exact partial-binding scenario). Do NOT read this as a bug -
 * later phases (5.2-5.6) add their own relay/attachment pairs to progressively
 * wire the remaining controls; the markup itself is never modified again.
 *
 * CRITICAL: Member declaration order prevents release build crashes.
 * Order: Relays -> WebView -> Attachments
 *
 * Destruction order (reverse of declaration):
 * 1. Attachments destroyed FIRST (stop using relays and WebView)
 * 2. WebView destroyed SECOND (safe, attachments are gone)
 * 3. Relays destroyed LAST (safe, nothing using them)
 *
 * NOTE ON DUAL-BINDING PAIR (filterCutoff): filterCutoff is bound to TWO
 * controls in v5-ui.html (base 'Cutoff' knob + ADV-revealed 'Cutoff Fine'
 * mirror knob) but gets exactly ONE relay + ONE attachment below - the JUCE
 * frontend library's getSliderState() caches state objects per parameter
 * name, so both DOM controls share the same underlying relay automatically.
 *
 * NOT BOUND THIS PHASE (documented ambiguity, resolved per task instructions):
 * - tone, bias, distortionMix: parameter-spec.md itself notes these have
 *   "Not yet bound to a mockup control" - no HTML element exists to bind,
 *   so no relay is created for them this phase either. Deferred to a future
 *   mockup iteration per parameter-spec.md's own Reconciliation Notes.
 * - "global bypass": the task brief mentions this, but no such parameter
 *   exists in the locked parameter-spec.md - the Center Master module's
 *   power icon in v5-ui.html is explicitly documented as decorative/
 *   non-bound (v5-ui.yaml line ~1031: "Center Master and Feedback+Delay
 *   show the same row with a decorative, non-bound power icon"). Resolved
 *   as: no global bypass control exists to wire; skipped.
 */

class CORRUPTRAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit CORRUPTRAudioProcessorEditor(CORRUPTRAudioProcessor&);
    ~CORRUPTRAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    /**
     * Resource provider (JUCE 8 required pattern)
     * Maps URLs to embedded binary data from Source/ui/public/
     */
    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    CORRUPTRAudioProcessor& processorRef;

    // ========================================================================
    // ⚠️ CRITICAL MEMBER DECLARATION ORDER ⚠️
    // Order: Relays -> WebView -> Attachments
    // Members are destroyed in REVERSE order of declaration.
    // ========================================================================

    // ------------------------------------------------------------------------
    // 1️⃣ RELAYS FIRST — Phase 5.1 scope only (14 of 94 total parameters)
    // ------------------------------------------------------------------------

    // --- Top Bar / Global ---
    std::unique_ptr<juce::WebComboBoxRelay> qualityModeRelay;

    // --- Distortion ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassSaturationRelay;
    std::unique_ptr<juce::WebComboBoxRelay> distortionAlgorithmRelay;
    std::unique_ptr<juce::WebSliderRelay> driveRelay;
    std::unique_ptr<juce::WebSliderRelay> foldRelay;
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassWaveshaperRelay;

    // --- Filter & EQ ---
    std::unique_ptr<juce::WebToggleButtonRelay> graphBypassFilterRelay;
    std::unique_ptr<juce::WebComboBoxRelay> filterTypeRelay;
    std::unique_ptr<juce::WebSliderRelay> filterCutoffRelay;
    std::unique_ptr<juce::WebSliderRelay> filterResonanceRelay;

    // --- Center Master (Output/Global) ---
    std::unique_ptr<juce::WebSliderRelay> mixRelay;
    std::unique_ptr<juce::WebComboBoxRelay> outputLimiterStyleRelay;
    std::unique_ptr<juce::WebSliderRelay> inputGainRelay;
    std::unique_ptr<juce::WebSliderRelay> outputGainRelay;

    // ------------------------------------------------------------------------
    // 2️⃣ WEBVIEW SECOND
    // ------------------------------------------------------------------------
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // ------------------------------------------------------------------------
    // 3️⃣ PARAMETER ATTACHMENTS LAST — matches relay list 1:1
    // ------------------------------------------------------------------------

    // --- Top Bar / Global ---
    std::unique_ptr<juce::WebComboBoxParameterAttachment> qualityModeAttachment;

    // --- Distortion ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassSaturationAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> distortionAlgorithmAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> driveAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> foldAttachment;
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassWaveshaperAttachment;

    // --- Filter & EQ ---
    std::unique_ptr<juce::WebToggleButtonParameterAttachment> graphBypassFilterAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> filterTypeAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> filterCutoffAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> filterResonanceAttachment;

    // --- Center Master (Output/Global) ---
    std::unique_ptr<juce::WebSliderParameterAttachment> mixAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> outputLimiterStyleAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> inputGainAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> outputGainAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CORRUPTRAudioProcessorEditor)
};
