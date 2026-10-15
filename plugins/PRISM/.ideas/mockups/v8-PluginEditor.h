#pragma once

// ============================================================================
// PRISM - WebView plugin editor (TEMPLATE generated from finalized mockup v8)
//
// This is reference boilerplate for Stage 3 (GUI). gui-agent adapts it to the
// real PluginProcessor; nothing here is copy-paste-final except the contract
// items listed below.
//
// CONTRACT WITH THE MOCKUP (v8-ui.html / v8-ui.yaml / parameter-spec.md)
//   JS -> C++   emitEvent("prismParam",   { id, value })      17 APVTS params,
//                                                              src1_name / src2_name,
//                                                              momentary: reset_all, reset_integrated
//               emitEvent("prismUiState", { row_splits[3], lower_col_splits[3], fullscreen, spectrogram_orientation })
//               emitEvent("prismReady",   {})                  page booted -> reply with "prismInit"
//   C++ -> JS   emitEvent("prismParamSync", { id, value })     host/automation/preset changes
//               emitEvent("prismInit",      { params{}, ui{} }) full resync after "prismReady"
//               emitEvent("prismFrame",     { ... })           analysis snapshot, ~60 Hz
//   v8 restyle (tiled panes, right sidebar with needle meter + 4 knobs + button grid) changed NO ids,
//   events or payloads; only the default layout splits and one optional frame field (bpm) differ from v7.
//   Seeding     GET /prism-init.js (resource provider) defines window.__PRISM_INIT_UI and
//               window.__PRISM_INIT_PARAMS BEFORE the page script runs.
//
// GESTURES: continuous controls send prismParam {id, gesture:'begin'} on pointerdown, value events while
//   dragging and {id, gesture:'end'} on release. C++ maps begin/end to begin/endChangeGesture and applies
//   value events with setValueNotifyingHost only. A value event with no open gesture (chips, toggles,
//   preset fan-out) is wrapped in its own begin/value/end. Open gestures are closed on prismReady and in
//   the destructor, and a repeated begin / stray end is ignored, so the host never sees unbalanced gestures.
// VISIBILITY / STATE RESYNC: nothing is consumed while the view is hidden; on regaining visibility, and
//   whenever the APVTS state is replaced (setStateInformation -> valueTreeRedirected), the editor re-pushes
//   the full "prismInit" (params, source names, splits, orientation, fullscreen) and re-applies the stored size.
//
// DESIGN NOTE - why event-based bindings instead of WebSliderRelay et al:
//   The mockup posts every control through the single prismParam event and
//   uses plain values (numbers for 1024/-48 style choices, strings for the
//   rest). That keeps one code path for all 17 parameters + the 2 string
//   state entries and avoids 17 relays. There are therefore NO relays or
//   WebXxxParameterAttachment objects. The member-order rule still applies in
//   spirit: everything that can call into the WebView (the ParamBridge and the
//   Timer) is declared AFTER webView so it is destroyed BEFORE it.
// ============================================================================

#include <JuceHeader.h>
#include "PluginProcessor.h"

#include <array>
#include <atomic>
#include <cmath>
#include <memory>

// ----------------------------------------------------------------------------
// Immutable analysis snapshot published by the processor's analysis worker.
// STAGE 2: move this block to Source/Analysis/AnalysisSnapshot.h (keep the
// PRISM_ANALYSIS_SNAPSHOT_DEFINED guard) and have PluginProcessor include it.
// Unavailable values are NaN / -inf and are sent to the UI as null (shown as
// dashes) - the UI never fabricates a measurement.
// ----------------------------------------------------------------------------
#ifndef PRISM_ANALYSIS_SNAPSHOT_DEFINED
#define PRISM_ANALYSIS_SNAPSHOT_DEFINED 1
namespace prism
{
    inline constexpr int kSourceCount        = 2;      // v1: main + 1 sidechain
    inline constexpr int kSpectrumPoints     = 640;    // log-spaced 20 Hz .. 20 kHz, dBFS (true, un-tilted)
    inline constexpr int kMaxWaveformColumns = 16;     // new columns per snapshot (84.33 columns/s nominal)
    inline constexpr int kGonioPairs         = 900;    // L,R pairs
    // Scope window must cover 20 ms (10 divisions x 2 ms) + up to 800 samples of rising-zero trigger search:
    //   192 kHz: 0.020 s * 192000 = 3840, + 800 = 4640 samples  (4096 was too short above ~164 kHz).
    // 8192 covers every host rate up to 192 kHz with headroom (and 352.8 kHz would need 7856).
    inline constexpr int kScopeSamples       = 8192;

    struct SourceSnapshot
    {
        bool  hasSpectrum = false;
        std::array<float, kSpectrumPoints> spectrumDb {};

        int   waveformColumnCount = 0;                        // each column: bass, mids, highs (0..1), flag (0 none, 1 beat, 2 bar)
        std::array<std::array<float, 4>, kMaxWaveformColumns> waveformColumns {};

        int   gonioPairCount = 0;
        std::array<float, 2 * kGonioPairs> gonio {};          // interleaved L,R in -1..1

        int   scopeCount = 0;
        std::array<float, kScopeSamples> scope {};            // time-domain window in -1..1

        float truePeakMaxDb   = -INFINITY;                    // max since reset_all
        float samplePeakMaxDb = -INFINITY;
        bool  hasStereo       = false;
        float correlation     = 0.0f;                         // -1..+1
        float widthPercent    = 0.0f;                         // 0..100
        float balanceDb       = 0.0f;
    };

    struct AnalysisSnapshot
    {
        std::uint64_t sequence     = 0;                       // strictly increasing; the editor skips repeats
        double        sampleRate   = 0.0;
        float         bpm          = NAN;                     // optional tempo for the waveform header annotation (NaN -> dashes)
        bool          signalPresent = false;                  // false -> UI shows "NO SIGNAL"

        float momentaryLufs  = NAN;                           // 400 ms
        float shortTermLufs  = NAN;                           // 3 s
        float integratedLufs = NAN;                           // NaN until gated data exists / after reset_integrated
        float loudnessRange  = NAN;                           // LU

        std::array<SourceSnapshot, kSourceCount> sources {};
    };
} // namespace prism
#endif

//==============================================================================
class PRISMAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    explicit PRISMAudioProcessorEditor (PRISMAudioProcessor& p);
    ~PRISMAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ParamBridge;   // APVTS + ValueTree listener -> lock-free dirty flags

    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    // JS -> C++
    void handleParamEvent   (const juce::var& payload);
    void handleUiStateEvent (const juce::var& payload);
    void handlePageReady();
    void closeOpenGestures();

    // C++ -> JS
    juce::var buildInitParams() const;
    juce::var buildInitUi() const;
    void pushFullInit();
    void flushParameterChanges();
    void pushAnalysisFrame();

    // ---- Stage 2 seams (stubbed so the editor compiles and the UI idles cleanly) ----
    std::shared_ptr<const prism::AnalysisSnapshot> fetchLatestSnapshot() const;
    void requestResetAll();
    void requestResetIntegrated();

    PRISMAudioProcessor& processorRef;
    juce::ComponentBoundsConstrainer constrainer;   // 55:36 aspect, min 770x504

    // ========================================================================
    // MEMBER DECLARATION ORDER (destroyed in reverse):
    //   webView first, then everything that talks to it.
    // ========================================================================

    // 1) WEBVIEW
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // 2) BRIDGE (declared after webView -> destroyed before it)
    std::unique_ptr<ParamBridge> paramBridge;

    std::array<bool, 17> gestureOpen {};    // per-parameter: beginChangeGesture issued, endChangeGesture pending (kParams order)
    bool wasHidden = false;                 // view was hidden at least once since the last full resync
    bool pageReady = false;                 // JS has registered its listeners ("prismReady" received)
    std::uint64_t lastSequence = 0;         // last snapshot sequence pushed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PRISMAudioProcessorEditor)
};
