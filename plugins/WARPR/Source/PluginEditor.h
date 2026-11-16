#pragma once
/*
  ==============================================================================
   WARPR - PluginEditor.h

   Adapted from the v9 mockup boilerplate (.ideas/mockups/v9-PluginEditor.h).
   Stage 1 status: the editor hosts the production page (Source/ui/public/index.html) and
   registers ALL 135 relays/attachments plus the 14 native functions against the STUB
   backend (UiBackendImpl), so it opens blank-safe with no "unknown to the backend"
   console warnings.  Full GUI wiring and verification is Stage 3 (phase G1).

   MEMBER ORDER (juce8-critical-patterns #11):
       relays  ->  webView  ->  attachments          (destroyed in reverse)
   ==============================================================================
*/

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>

#include "PluginProcessor.h"

//==============================================================================
// Parameter tables.  X(id) -> member names <id>Relay / <id>Attachment and the APVTS/relay
// name "<id>".  These tables are verified against Parameters/ParamTable.h (the single source
// of truth) by WARPR_Tests: 37 + 5 + 13 = 55 here, plus 10 x 8 LFO parameters = 135.
//==============================================================================
#define WARPR_SLIDER_PARAMS(X) \
    X(sampleStart) X(sampleEnd) X(rootNote) X(transpose) X(fineTune) X(grainPosition) \
    X(grainScan) X(grainScanDist) X(grainSize) X(grainShape) X(grainVariation) \
    X(grainSpread) X(grainReverse) X(grainDensity) X(warpAmount) X(warpParamA) \
    X(warpParamB) X(modOscRatio) X(warpMix) X(shaperMorph) X(shaperMix) X(ampAttack) \
    X(ampDecay) X(ampSustain) X(ampRelease) X(filterCutoff) X(filterResonance) \
    X(filterDrive) X(filterMorph) X(filterKeyTrack) X(filterMix) X(polyphony) \
    X(unisonVoices) X(unisonDetune) X(unisonSpread) X(glideTime) X(masterGain)

#define WARPR_TOGGLE_PARAMS(X) \
    X(grainScanLoop) X(grainScanKey) X(warpEnable) X(shaperEnable) X(filterEnable)

#define WARPR_COMBO_PARAMS(X) \
    X(loopMode) X(direction) X(playMode) X(grainMode) X(warpMode) X(modOscSource) \
    X(oversampling) X(shaperCurveSelect) X(shaperPreset) X(shaperSymmetry) X(filterType) \
    X(voiceMode) X(bendRange)

//==============================================================================
class PluginEditor : public juce::AudioProcessorEditor,
                     public juce::FileDragAndDropTarget,
                     private juce::Timer
{
public:
    explicit PluginEditor (PluginProcessor& p);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // drop a .wav/.aif/.aiff on the editor to load it as the sample
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    static constexpr int designWidth  = 1200;
    static constexpr int designHeight = 820;
    static constexpr int numLfos      = 10;

private:
    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    void registerNativeFunctions (juce::WebBrowserComponent::Options& options);
    void emitViz (const WarprVizFrame&);
    void emitSample();
    void emitFilterResponse();
    void emitShaperCurves();
    void emitUiState();
    void emitPreset();

    PluginProcessor& audioProcessor;

    // WebView drag & drop: WKWebView swallows OS file drops before JUCE sees them, so the page
    // catches the drop and streams the file's bytes over the bridge (dropBegin/Chunk/End).
    std::unique_ptr<juce::MemoryOutputStream> dropStream;
    juce::String dropName;

    // ========================================================================
    // CRITICAL MEMBER DECLARATION ORDER:  Relays -> WebView -> Attachments
    // Attachments are destroyed first (they talk to the WebView), then the
    // WebView, then the relays it references.  Do NOT reorder.
    // ========================================================================

    // 1. RELAYS ---------------------------------------------------------------
   #define X(id) std::unique_ptr<juce::WebSliderRelay>       id##Relay;
    WARPR_SLIDER_PARAMS (X)
   #undef X
   #define X(id) std::unique_ptr<juce::WebToggleButtonRelay> id##Relay;
    WARPR_TOGGLE_PARAMS (X)
   #undef X
   #define X(id) std::unique_ptr<juce::WebComboBoxRelay>     id##Relay;
    WARPR_COMBO_PARAMS (X)
   #undef X

    struct LfoRelays   // lfo<N>Mode/Dir/Sync/Rate/Phase/Smooth/GridX/GridY
    {
        std::unique_ptr<juce::WebComboBoxRelay> mode, dir, sync, gridX, gridY;
        std::unique_ptr<juce::WebSliderRelay>   rate, phase, smooth;
    };
    std::array<LfoRelays, (size_t) numLfos> lfoRelays;

    // 2. WEBVIEW --------------------------------------------------------------
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // 3. ATTACHMENTS ----------------------------------------------------------
   #define X(id) std::unique_ptr<juce::WebSliderParameterAttachment>       id##Attachment;
    WARPR_SLIDER_PARAMS (X)
   #undef X
   #define X(id) std::unique_ptr<juce::WebToggleButtonParameterAttachment> id##Attachment;
    WARPR_TOGGLE_PARAMS (X)
   #undef X
   #define X(id) std::unique_ptr<juce::WebComboBoxParameterAttachment>     id##Attachment;
    WARPR_COMBO_PARAMS (X)
   #undef X

    struct LfoAttachments
    {
        std::unique_ptr<juce::WebComboBoxParameterAttachment> mode, dir, sync, gridX, gridY;
        std::unique_ptr<juce::WebSliderParameterAttachment>   rate, phase, smooth;
    };
    std::array<LfoAttachments, (size_t) numLfos> lfoAttachments;

    // Everything below is destroyed BEFORE the attachments (safe: no WebView dependency)
    std::unique_ptr<juce::FileChooser> chooser;
    uint32_t seenUiState = ~0u, seenSample = ~0u, seenShaper = ~0u, seenFilter = ~0u, seenPreset = ~0u;
    std::atomic<bool> sampleRequested { true }, filterRequested { true };
    juce::String lastPresetName;
    bool playheadsWereShown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
