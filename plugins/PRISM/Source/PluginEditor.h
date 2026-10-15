#pragma once

// ============================================================================
// PRISM - WebView plugin editor (adapted from finalized mockup v8)
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

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include "Analysis/AnalysisSnapshot.h"   // prism::AnalysisSnapshot (moved here in Stage 1)

// Optional per-frame timing log (Phase 3.3 throughput gate). OFF by default.
// Debug-only remote control: when ON, the timer executes JS read from the file named by $PRISM_DEBUG_JS_FILE
// (deleted after reading) and prints the result to stderr. OFF by default; never ship ON.
#ifndef PRISM_DEBUG_CONTROL
 #define PRISM_DEBUG_CONTROL 0
#endif

#ifndef PRISM_FRAME_TIMING
 #define PRISM_FRAME_TIMING 0
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
    void visibilityChanged() override;

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
    bool pushAnalysisFrameImpl();   // true when a new snapshot was pushed

    // ---- Stage 2 seams (stubbed so the editor compiles and the UI idles cleanly) ----
    std::shared_ptr<const prism::AnalysisSnapshot> fetchLatestSnapshot() const;
    void requestResetAll();
    void requestResetIntegrated();
    void setEngineActive (bool active);   // idempotent wrapper over processorRef.setEditorActive (no double counting)

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
    std::uint64_t lastSequence = 0;
    bool engineActive = false;              // this editor currently counts as "active" in the processor
    std::uint32_t tickCount = 0;            // 60 Hz timer ticks (reduced_motion halves the push rate, gonio/scope decimation)

   #if PRISM_FRAME_TIMING
    std::vector<float> frameTimesMs;        // pushAnalysisFrame wall time (ms), logged every 1200 frames
   #endif         // last snapshot sequence pushed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PRISMAudioProcessorEditor)
};
