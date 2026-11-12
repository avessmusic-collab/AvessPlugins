#pragma once
/*
  ==============================================================================
   WARPR v9 - PluginEditor.h  (Stage 3 reference boilerplate, NOT a drop-in)

   Generated for mockup v9.  Class names follow the repo convention
   (PluginEditor / PluginProcessor); adapt to the real processor class names
   (WARPR is a working title).  Parameter IDs are generated from
   .ideas/parameter-spec.md - 135 parameters (62 Float + 5 Int + 5 Bool + 63 Choice).

   MEMBER ORDER (juce8-critical-patterns #11 / member-order rule):
       relays  ->  webView  ->  attachments          (destroyed in reverse)

   WHAT THE PROCESSOR MUST PROVIDE (see WarprUiBackend below):
       juce::AudioProcessorValueTreeState apvts;
       WarprUiBackend& getUiBackend();
   ==============================================================================
*/

#include <JuceHeader.h>
#include "PluginProcessor.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

//==============================================================================
// Lock-free audio-thread -> UI data path (SPSC).  Move this block to
// Source/DSP/VizFifo.h in Stage 3 so PluginProcessor.h can include it too.
//==============================================================================
struct WarprVizFrame
{
    static constexpr int maxVoices = 32;
    static constexpr int numLfos   = 10;

    // organism drivers (0..1 unless noted) - computed in the audio thread from the POST-WARP stereo bus
    float low = 0, high = 0, rms = 0;       // band energies / overall level (smoothed envelope followers)
    float theta = 0;                        // stereo phase angle, radians (atan2(side, mid) of block energy)
    float r = 0;                            // stereo vector length 0..1
    float transient = 0;                    // strength of the latest detected transient (0 = none this frame)
    float tang = 0;                         // radians: angle on the organism where the swell appears (spectral centroid -> 0..2pi)
    uint32_t tseq = 0;                      // increments once per detected transient (UI diffs this)

    // meters (linear peak since the previous frame, post master gain)
    float peakL = 0, peakR = 0;

    // per-voice sample read positions for the waveform playheads
    int numVoices = 0;
    struct Voice { float pos = 0, level = 0; };            // pos 0..1 absolute in the sample
    std::array<Voice, maxVoices> voices{};

    // LFO phases 0..1 of the most recent voice (-1 = LFO idle / unused)
    std::array<float, numLfos> lfoPhase{ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
};

class WarprVizFifo
{
public:
    // AUDIO THREAD: wait-free, never allocates.  Drops the frame if the UI is behind.
    void push (const WarprVizFrame& f) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0) { slots[(size_t) s1] = f; fifo.finishedWrite (1); }
    }
    // UI THREAD: drains everything, returns the newest frame (if any)
    bool popLatest (WarprVizFrame& out) noexcept
    {
        bool got = false;
        while (fifo.getNumReady() > 0)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 > 0) { out = slots[(size_t) s1]; got = true; }
            fifo.finishedRead (n1 + n2);
        }
        return got;
    }
private:
    static constexpr int capacity = 64;
    juce::AbstractFifo fifo { capacity };
    std::array<WarprVizFrame, (size_t) capacity> slots{};
};

//==============================================================================
// What the editor needs from the processor.  Implemented in Stage 2/3 by
// PluginProcessor (or a helper owned by it).  Every method except vizFifo()
// is called on the MESSAGE thread only.
//==============================================================================
struct WarprSampleDisplay
{
    juce::String name;                       // "" = nothing loaded (editor shows NO SAMPLE)
    int numChannels = 0;
    double sampleRate = 0;
    juce::int64 numSamples = 0;
    std::vector<float> peaksMinMax;          // [min0,max0,min1,max1,...] in -1..1, <= 2048 pairs, mono mix
};

class WarprUiBackend
{
public:
    enum class Revision { uiState, sample, shaperCurves, filterResponse, preset };

    virtual ~WarprUiBackend() = default;

    virtual WarprVizFifo& vizFifo() = 0;                                    // audio thread pushes, editor pops

    // monotonically increasing counters; the editor timer re-emits the matching event when one changes.
    // uiState/sample/preset: bump on setStateInformation + preset load + sample load.
    // shaperCurves: bump ONLY for non-UI-originated changes (preset param, reset, normalize, state load)
    //   - never for setShaperCurveJson, or the UI would receive an echo mid-drag.
    // filterResponse: bump when any filter parameter or the filter state changes.
    virtual uint32_t revision (Revision) const = 0;

    // non-parameter state (see parameter-spec.md "Non-Parameter State")
    virtual juce::String getUiStateJson() = 0;                              // {lfoCount,lfoShapes[],modMatrix[],shaperCurves{A,B}}
    virtual void setLfoCount (int n) = 0;
    virtual void setLfoShapeJson (int index, const juce::String& json) = 0;
    virtual void setModMatrixJson (const juce::String& json) = 0;
    virtual void setShaperCurveJson (const juce::String& curveId, const juce::String& json) = 0;
    virtual juce::String getShaperCurveJson (const juce::String& curveId) = 0;
    virtual void shaperReset (const juce::String& curveId) = 0;
    virtual void shaperNormalize (const juce::String& curveId) = 0;

    // sample (decode OFF the audio thread inside; publish by bumping Revision::sample)
    virtual void loadSampleFile (const juce::File& file) = 0;
    virtual WarprSampleDisplay getSampleDisplay() = 0;

    // real filter magnitude response: fills n log-spaced points 20 Hz..20 kHz in dB from the actual filter DSP
    virtual void computeFilterResponse (float* dbOut, int n) = 0;

    // presets
    virtual juce::String getPresetName() = 0;
    virtual void presetStep (int delta) = 0;
    virtual void presetSaveDialog() = 0;
    virtual void presetLoadDialog() = 0;
    virtual void openSettings() = 0;
};

//==============================================================================
// Parameter tables (generated from parameter-spec.md).  X(id) -> member names
// <id>Relay / <id>Attachment and the APVTS/relay name "<id>".
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
