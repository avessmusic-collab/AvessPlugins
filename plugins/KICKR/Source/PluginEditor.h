#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/KickrLookAndFeel.h"
#include "UI/KickrKnob.h"
#include "UI/WaveformDisplay.h"
#include "UI/SpectrumDisplay.h"
#include "UI/SampleWaveformView.h"

class KICKRAudioProcessor;

/**
    Stage 3 Phase 3.1 — native JUCE editor (NOT WebView).

    Mirrors the approved v2 mockup (`plugins/KICKR/.ideas/mockups/v1-ui-concept.html`):
    soft slate-grey chassis + recessed faceplate, header, an oscilloscope PLACEHOLDER
    (real analyzer is Phase 3.2), the SAMPLE strip, four PUNCH/BODY/CRUSH/TAIL macro
    modules and a 7-cell engine strip. Every one of the 60 APVTS parameters is bound
    exactly once via SliderAttachment / ButtonAttachment / ComboBoxAttachment.

    Resizable, fixed 1600:1170 aspect; all layout scales from getLocalBounds() through a
    single `getWidth() / 1600` factor.
*/
class KICKRAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        public  juce::FileDragAndDropTarget,
                                        private juce::ChangeListener
{
public:
    explicit KICKRAudioProcessorEditor (KICKRAudioProcessor&);
    ~KICKRAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    // ---- Phase 3.3: drag an audio file anywhere on the editor -> managed bank ----
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit  (const juce::StringArray&) override;
    void filesDropped  (const juce::StringArray& files, int, int) override;

    /** Headless snapshot tests only — pull one analyzer frame into the displays. */
    void refreshAnalyzersForSnapshot() { waveDisplay.refreshNow(); spectrumDisplay.refreshNow(); }
    /** Test hook — run the drop path for one file. */
    void importDroppedFileForTest (const juce::File& f) { importAudioFile (f); }

    /** Test hook — how many of the sample-tweak controls currently report enabled
        (via the real Component::isEnabled() chain, not a mirrored flag). Should be
        0 while sampleEnable is off and sampleTweaksCountForTest() once it's on. */
    int countEnabledSampleTweaksForTest() const noexcept
    {
        return (int) std::count_if (sampleTweaks.begin(), sampleTweaks.end(),
                                    [] (juce::Component* c) { return c != nullptr && c->isEnabled(); });
    }
    int sampleTweaksCountForTest() const noexcept { return (int) sampleTweaks.size(); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // ---- Phase 3.3 ----
    void importAudioFile (const juce::File&);
    void loadPresetAt (int combinedIndex);
    void rebuildPresetList();
    void showPresetMenu();
    void showSaveDialog();
    void doRandomize();
    void doMutate();
    void doAB();
    void syncPresetName();
    void showNotice (const juce::String&);
    void clearNotice();

    kickr::KickrKnob&   addKnob   (juce::StringRef id, const juce::String& caption,
                                   juce::Colour accent, bool bipolar, kickr::KickrKnob::Size);
    kickr::KickrToggle& addToggle (juce::StringRef id, const juce::String& text, juce::Colour accent);
    juce::ComboBox&     addCombo  (juce::StringRef id, const juce::StringArray& items, juce::Colour accent);
    juce::Label&        addSectionLabel (const juce::String& text, juce::Colour accent);

    void layoutHeader (juce::Rectangle<int>);
    void layoutScope  (juce::Rectangle<int>);
    void layoutSample (juce::Rectangle<int>);
    void layoutHeroes (juce::Rectangle<int>);
    void layoutEngine (juce::Rectangle<int>);
    void layoutMorph (juce::Rectangle<int>);   // 2026-09-01 — Morph knob, left of the scope panel

    void updateSampleLabel();
    void updateUndoRedoState();

    static void placeRow  (juce::Rectangle<int> area, const std::vector<juce::Component*>&, int gap);
    static void placeGrid (juce::Rectangle<int> area, const std::vector<juce::Component*>&, int cols, int gap);

    int scaled (int referencePx) const noexcept;

    KICKRAudioProcessor& proc;

    kickr::KickrLookAndFeel lnf;
    // 2026-08-31 (user request): hover tooltips appear after 3 s, not 0.5 s.
    // 2026-09-01 (user request): shortened to 1.5 s. Keep in sync with the matching
    // setMillisecondsBeforeTipAppears() call in the editor constructor.
    static constexpr int kTooltipDelayMs = 1500;
    juce::TooltipWindow tooltip { this, kTooltipDelayMs };

    std::vector<std::unique_ptr<kickr::KickrKnob>>   knobs;
    std::vector<std::unique_ptr<kickr::KickrToggle>> toggles;
    std::vector<std::unique_ptr<juce::ComboBox>>     combos;
    std::vector<std::unique_ptr<juce::ComboBoxParameterAttachment>> comboAtts;
    std::vector<std::unique_ptr<juce::Label>>        sectionLabels;

    int boundParamCount { 0 };

    // ---- header ----
    juce::Label wordmark, versionLabel, presetNameLabel, noticeLabel;
    juce::TextButton undoButton   { "Undo" };
    juce::TextButton redoButton   { "Redo" };
    juce::TextButton saveButton   { "Save" };
    juce::TextButton randomButton { "Random" };
    juce::TextButton mutateButton { "Mutate" };
    juce::TextButton abButton     { "A / B" };
    juce::TextButton presetPrev   { juce::String::fromUTF8 ("\xe2\x97\x84") };
    juce::TextButton presetNext   { juce::String::fromUTF8 ("\xe2\x96\xba") };

    // ---- oscilloscope (Phase 3.2 — real-time analyzers) ----
    juce::TextButton scopeWaveButton     { "WAVE" };
    juce::TextButton scopeSpectrumButton { "SPECTRUM" };
    kickr::WaveformDisplay waveDisplay     { proc.getAnalyzer() };
    kickr::SpectrumDisplay spectrumDisplay { proc.getAnalyzer() };

    // 2026-09-01 (user request) — Morph: body oscillator waveform morph, left of the scope.
    kickr::KickrKnob* morphKnob { nullptr };

    // ---- sample strip ----
    juce::Label      sampleNameLabel, sampleDropHint;
    juce::TextButton samplePrev { juce::String::fromUTF8 ("\xe2\x97\x84") };
    juce::TextButton sampleNext { juce::String::fromUTF8 ("\xe2\x96\xba") };
    std::array<juce::Component*, 2> sampleToggles {};
    std::vector<juce::Component*>   sampleTweaks;
    // 2026-09-01 (user request): grey out + disable sampleTweaks while sampleEnable is
    // off. juce::ParameterAttachment marshals both UI clicks and host automation to the
    // message thread for us, so the callback can touch components directly (no timer).
    std::unique_ptr<juce::ParameterAttachment> sampleEnableWatcher;
    // Small static preview of the currently loaded sample file (user request 2026-08-31)
    // — not the live scope; shows the raw file + the sampleStart/sampleEnd trim window.
    kickr::SampleWaveformView sampleWaveform { proc.getValueTreeState() };

    // ---- macro modules ----
    std::array<juce::Label*, 4>     heroTitles {};
    std::array<juce::Component*, 4> heroBig {};
    std::array<juce::Component*, 4> heroMacro {};
    std::vector<juce::Component*>   punchFooter, bodyFooter, crushFooter, tailFooter;

    // ---- engine strip ----
    std::array<juce::Label*, 7>                  engineTitles {};
    std::array<juce::Component*, 7>              engineCombos {};
    std::array<juce::Component*, 7>              engineExtra  {};
    std::array<std::vector<juce::Component*>, 7> engineCells;

    // cached bounds for paint()
    juce::Rectangle<int> faceplateBounds, scopeBounds, sampleBounds, engineBounds;
    std::array<juce::Rectangle<int>, 4> heroRects {};
    std::array<juce::Rectangle<int>, 7> engineRects {};

    float scaleFactor { 1.0f };

    // ---- Phase 3.3 preset / A-B state ----
    juce::StringArray  presetList;        // factory names, then user names
    int               numFactoryInList { 0 };
    juce::MemoryBlock  abSlot[2];
    int               abActive { 0 };
    bool              fileDragActive { false };
    std::unique_ptr<juce::AlertWindow> saveDialog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KICKRAudioProcessorEditor)
};
