#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

/**
 * DROP WebView-based Plugin Editor.
 *
 * Adapted from the finalized mockup v10 "Light Neumorphic / White Marble"
 * template (plugins/DROP/.ideas/mockups/v10-PluginEditor.h/.cpp,
 * v10-integration-checklist.md) for Stage 3 GUI Phase 4.1 ("Layout and
 * Basic Controls"). Every `processorRef.getXxx()` call in the original
 * template was a NAMED ASSUMPTION about the eventual PluginProcessor API;
 * all have been cross-checked against the real
 * plugins/DROP/Source/PluginProcessor.h and corrected below (see
 * PluginEditor.cpp's doc comments at each call site for what changed).
 *
 * UI Mockup: v10 "Light Neumorphic / White Marble" (finalized 2026-09-14).
 * Window 1000x560 base, resizable, min 800x450, max 1400x790
 * (v10-ui.yaml `window` block; matches Stage 1's placeholder editor sizing
 * exactly, so no host-visible size jump on this swap).
 * Full contract: plugins/DROP/.ideas/parameter-spec.md (locked, 19 APVTS
 * parameters + non-parameter ValueTree/atomic state for trigger mode / MIDI
 * note / Chaos seed / custom tension curve, per architecture.md).
 *
 * ============================================================================
 * CRITICAL: Member declaration order prevents release build crashes.
 * Order: Relays -> WebView -> Attachments
 * (juce8-critical-patterns.md #11)
 *
 * Destruction order (reverse of declaration):
 * 1. Attachments destroyed FIRST (stop using relays and WebView)
 * 2. WebView destroyed SECOND (safe, attachments are gone)
 * 3. Relays destroyed LAST (safe, nothing using them)
 * ============================================================================
 *
 * PARAMETER COVERAGE: all 21 APVTS parameters get a Relay + Attachment pair
 * below (trigger=toggle, buildLength/style=combo, the remaining 18=slider),
 * matching plugins/DROP/.ideas/parameter-spec.md exactly. The 20th, `tail`,
 * is a POST-1.0 addition (2026-09-16, user-directed improvement - see
 * PluginProcessor.h's "POST-1.0 IMPROVEMENT" doc comment) appended AFTER
 * the original 19 (not interleaved), matching createParameterLayout()'s own
 * append-only ordering. The 21st, `noiseTension`, is a POST-1.0 FOURTH
 * IMPROVEMENT SESSION addition (2026-09-17, user-directed - see
 * PluginProcessor.h's own top doc comment) appended AFTER `tail`, same
 * append-only convention.
 * `triggerMode` is deliberately NOT a Relay/Attachment pair - per
 * architecture.md it is NON-PARAMETER state (manual/MIDI/auto-sync is a
 * discrete user preference, not something a host automates), so it is
 * bridged via two native functions instead ("setTriggerMode" JS->C++,
 * "getInitialUIState" JS->C++ one-shot read at load) - see v10-ui.html's
 * own top-of-script doc comment for the JS side.
 *
 * POST-1.0 (2026-09-17, user-directed: "remove midi note selection"): the
 * MIDI-trigger-note picker ("MIDI NOTE" mini-screen chip, #midiNoteChip,
 * added Stage 3 GUI Phase 4.2) and its "setMidiTriggerNote" native-function
 * bridge are REMOVED from this class - DOCUMENTED SUPERSESSION. MIDI trigger
 * mode now responds to ANY note-on message (see PluginProcessor.cpp's
 * processBlock() doc comment) rather than a user-configurable one, so no
 * note-picker UI is needed. `getInitialUIState`'s response no longer
 * includes a `midiTriggerNote` field. `DROPAudioProcessor::
 * getMidiTriggerNote()`/`setMidiTriggerNote()` remain on the processor
 * (unused by this editor now) purely for saved-session backward
 * compatibility - see PluginProcessor.h's doc comment.
 *
 * VISUALIZATION (Stage 3 GUI Phase 4.3, COMPLETE): a private juce::Timer
 * (30Hz) drains DROPAudioProcessor::popVisualizationFrame() (the real
 * ring-buffer accessor - NOT the v10 template's assumed
 * getLatestVisualizationSnapshot()) every tick, keeping only the most
 * recently pushed frame and emitting exactly ONE "dropVisUpdate" JSON
 * payload per tick via webView->emitEventIfBrowserIsVisible() - matching
 * this repo's established pattern (see CORRUPTR's PluginEditor.h "PHASE 5.6
 * ADDITIONS" doc comment for the precedent this follows). The processor
 * side (pushVisualizationFrame()) silently, harmlessly drops a frame if the
 * bounded 256-entry FIFO is ever full (GUI not draining fast enough, or not
 * yet attached) - never blocks the audio thread.
 *
 * Editor-reopen correctness (plan.md criterion: reopening the editor
 * mid-transition must show the CURRENT state immediately, not wait for the
 * first ~33ms Timer tick / show stale Idle): the SAME drain-all-keep-latest
 * logic (buildDropVisUpdateVar()) is also invoked once, synchronously, from
 * the "getInitialUIState" native function's one-shot load-time response
 * (nested under a "visualization" key) - see PluginEditor.cpp's doc comment
 * at that call site.
 */

class DROPAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    explicit DROPAudioProcessorEditor(DROPAudioProcessor&);
    ~DROPAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    /**
     * Resource provider (JUCE 8 required pattern, juce8-critical-patterns.md #8)
     * Explicit URL -> BinaryData mapping for everything under Source/ui/public/.
     */
    std::optional<juce::WebBrowserComponent::Resource> getResource(const juce::String& url);

    // ------------------------------------------------------------------------
    // Non-APVTS "trigger mode" bridge (architecture.md: manual/MIDI/auto-sync
    // is non-parameter state, not an APVTS parameter - see this header's own
    // top doc comment). Plain helper methods, not stored Relay/WebView/
    // Attachment objects, so their declaration position does not participate
    // in the destruction-order contract below.
    //
    // CORRECTED from the v10-PluginEditor.h template's ASSUMPTION: the real
    // PluginProcessor does NOT expose a child-ValueTree accessor
    // (`getCustomStateTree()`) - trigger mode is a plain
    // `std::atomic<int> triggerMode` on DROPAudioProcessor, read/written via
    // `getTriggerMode()`/`setTriggerMode()` (see PluginProcessor.h's
    // `TriggerMode` enum + accessors, and getStateInformation()'s
    // `<DropCustomState triggerMode="...">` persistence). Adjusted below.
    // ------------------------------------------------------------------------
    int getTriggerModeIndex() const;   // 0=manual, 1=midi, 2=auto
    void setTriggerModeIndex(int index);

    // ------------------------------------------------------------------------
    // 30Hz visualization push (see this header's top doc comment,
    // "VISUALIZATION"). juce::Timer override - drains the processor's
    // lock-free SPSC ring buffer via popVisualizationFrame() (message-
    // thread-safe) and pushes one compact "dropVisUpdate" JSON payload via
    // emitEventIfBrowserIsVisible(). Never touches audio-thread state
    // directly.
    // ------------------------------------------------------------------------
    void timerCallback() override;
    juce::var buildDropVisUpdateVar();

    // Cache of the most recently drained ring-buffer frame (see
    // buildDropVisUpdateVar()'s doc comment) - lets a timer tick that finds
    // the FIFO empty (or the editor being reopened mid-transition) keep
    // reporting the last known real state instead of snapping to zero.
    DROPAudioProcessor::VisualizationFrame lastVisualizationFrame {};

    // Mirrors drop::DropState's enum order exactly (architecture.md:
    // "enum class DropState { Idle, Arming, Building, Peak, Impact,
    // Recovery }" - see Source/DSP/DropStateMachine.h for the authoritative
    // declaration). If DSP ever reorders the enum, this switch must be
    // updated in lockstep (v10-integration-checklist.md Section 8).
    static juce::String dropStateToString(int dropStateEnumValue);

    DROPAudioProcessor& processorRef;

    // ========================================================================
    // CRITICAL MEMBER DECLARATION ORDER
    // Order: Relays -> WebView -> Attachments
    // Members are destroyed in REVERSE order of declaration.
    // ========================================================================

    // ------------------------------------------------------------------------
    // 1) RELAYS FIRST - all 20 APVTS parameters (parameter-spec.md order;
    // `tail` is the POST-1.0 20th, appended after `output` below)
    // ------------------------------------------------------------------------

    // --- Trigger / Timing / Style (Main page) ---
    std::unique_ptr<juce::WebToggleButtonRelay> triggerRelay;
    std::unique_ptr<juce::WebComboBoxRelay> buildLengthRelay;
    std::unique_ptr<juce::WebComboBoxRelay> styleRelay;

    // --- Hero macros (Main page) ---
    std::unique_ptr<juce::WebSliderRelay> intensityRelay;
    std::unique_ptr<juce::WebSliderRelay> dryWetRelay;

    // --- Shape (Advanced page) ---
    std::unique_ptr<juce::WebSliderRelay> tensionRelay;
    std::unique_ptr<juce::WebSliderRelay> chaosRelay;
    std::unique_ptr<juce::WebSliderRelay> impactRelay;

    // --- 10 module macros (Advanced page) ---
    std::unique_ptr<juce::WebSliderRelay> filterRelay;
    std::unique_ptr<juce::WebSliderRelay> widthRelay;
    std::unique_ptr<juce::WebSliderRelay> pitchRelay;
    std::unique_ptr<juce::WebSliderRelay> gateRelay;
    std::unique_ptr<juce::WebSliderRelay> stutterRelay;
    std::unique_ptr<juce::WebSliderRelay> delayRelay;
    std::unique_ptr<juce::WebSliderRelay> reverbRelay;
    std::unique_ptr<juce::WebSliderRelay> distortionRelay;
    std::unique_ptr<juce::WebSliderRelay> digitalRelay;
    std::unique_ptr<juce::WebSliderRelay> noiseRelay;

    // --- Output (Advanced page) ---
    std::unique_ptr<juce::WebSliderRelay> outputRelay;

    // --- Tail (Advanced page, POST-1.0 addition - see this header's top doc comment) ---
    std::unique_ptr<juce::WebSliderRelay> tailRelay;

    // --- Noise Tension (Advanced page, POST-1.0 FOURTH IMPROVEMENT SESSION
    // addition - see this header's top doc comment / PluginProcessor.h) ---
    std::unique_ptr<juce::WebSliderRelay> noiseTensionRelay;
    std::unique_ptr<juce::WebComboBoxRelay> noiseTypeRelay; // POST-1.0 SEVENTH IMPROVEMENT SESSION (2026-09-18)

    // ------------------------------------------------------------------------
    // 2) WEBVIEW SECOND
    // ------------------------------------------------------------------------
    std::unique_ptr<juce::WebBrowserComponent> webView;

    // ------------------------------------------------------------------------
    // 3) ATTACHMENTS LAST - matches relay list 1:1 (20 of 20)
    // ------------------------------------------------------------------------

    std::unique_ptr<juce::WebToggleButtonParameterAttachment> triggerAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> buildLengthAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> styleAttachment;

    std::unique_ptr<juce::WebSliderParameterAttachment> intensityAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> dryWetAttachment;

    std::unique_ptr<juce::WebSliderParameterAttachment> tensionAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> chaosAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> impactAttachment;

    std::unique_ptr<juce::WebSliderParameterAttachment> filterAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> widthAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> pitchAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> gateAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> stutterAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> delayAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> reverbAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> distortionAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> digitalAttachment;
    std::unique_ptr<juce::WebSliderParameterAttachment> noiseAttachment;

    std::unique_ptr<juce::WebSliderParameterAttachment> outputAttachment;

    // POST-1.0 addition (see this header's top doc comment).
    std::unique_ptr<juce::WebSliderParameterAttachment> tailAttachment;

    // POST-1.0 FOURTH IMPROVEMENT SESSION addition (see this header's top doc comment).
    std::unique_ptr<juce::WebSliderParameterAttachment> noiseTensionAttachment;
    std::unique_ptr<juce::WebComboBoxParameterAttachment> noiseTypeAttachment; // POST-1.0 SEVENTH IMPROVEMENT SESSION (2026-09-18)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DROPAudioProcessorEditor)
};
