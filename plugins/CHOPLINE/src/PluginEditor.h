// CHOPLINE — PluginEditor (Phases 1–5)
// Native-JUCE editor on the locked v17 wireframe-light design. MAIN page:
// header (wordmark, Phase 5 preset browser, MIDI dot, MAIN|FX, gear) +
// Strip A sampler + Strip B grid (with the Phase 5 lane editor) + Beat
// Repeat / Master cards. FX page: CHOP column (PLAYBACK + ENVELOPE) + MASTER
// column. Both pages stay alive (setVisible toggling) so playhead/meters
// keep running (v17 note_for_production). The chop detail overlay (sample
// editor) opens on double-click of a filled box / strip chop, the '…' chop
// button, or Enter.
// Keyboard (v17 map): Tab toggles the page, M/F jump, 1–8 select the chop,
// H = momentary Beat Repeat hold, SPACE = momentary transport preview
// (Standalone free-run gate, Phase 5), Enter opens / Esc closes the overlay,
// Cmd/Ctrl-Z undo. The whole UI is laid out on a fixed 1100x720 canvas that
// scales 75%–200% with the window (fixed aspect).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "state/Exporter.h"
#include "state/PresetManager.h"
#include "ui/BeatRepeatPanel.h"
#include "ui/ChopEditorOverlay.h"
#include "ui/FxPage.h"
#include "ui/GridStrip.h"
#include "ui/LookAndFeel.h"
#include "ui/MasterPanel.h"
#include "ui/SamplerStrip.h"

namespace chopline
{

/// Phase 5 drag-out backend (spec §10): writes the pattern MIDI temp file on
/// the message thread and renders the pattern WAV on a background thread
/// through a private offline engine (state/Exporter). Temp files live in a
/// per-instance folder of the system temp dir, cleaned up on destruction.
class PatternDragExporter final : private juce::Thread
{
public:
    explicit PatternDragExporter (ChoplineAudioProcessor& p);
    ~PatternDragExporter() override;

    juce::File writeMidiFile();        // message thread (drag start)
    void startRender();                // message thread; always renders fresh
    juce::File getRenderedFile() const; // {} while rendering / failed
    float getProgress() const;         // <0 idle, 0..1 rendering

private:
    void run() override;

    ChoplineAudioProcessor& processor;
    juce::File tempDir;
    juce::File wavFile;

    // Captured on the message thread at startRender, read by run().
    juce::ValueTree capturedState;
    state::OfflineRenderSpec capturedSpec;

    std::atomic<float> progress { -1.0f };
    std::atomic<bool> ready { false };
};

class ChoplineAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           public juce::DragAndDropContainer
{
public:
    explicit ChoplineAudioProcessorEditor (ChoplineAudioProcessor&);
    ~ChoplineAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override; // H = momentary BR hold (v17)
    void focusLost (FocusChangeType) override;      // releases a stuck H hold

private:
    // ---- small header widgets ------------------------------------------------
    class MidiActivityDot final : public juce::Component
    {
    public:
        explicit MidiActivityDot (engine::Engine& e) : engine (e) {}
        void tick(); // from the VBlank callback
        void paint (juce::Graphics&) override;

    private:
        engine::Engine& engine;
        std::uint32_t lastCount = 0;
        juce::uint32 flashStartMs = 0;
        bool lit = false;
    };

    class PageSeg final : public juce::Component // MAIN | FX (v17 page_nav)
    {
    public:
        std::function<int()> getPage;
        std::function<void (int)> setPage;
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
    };

    class GearButton final : public juce::Button
    {
    public:
        GearButton() : juce::Button ("Settings") {}
        void paintButton (juce::Graphics&, bool, bool) override;
    };

    class HoldBadge final : public juce::Component // REPEAT HOLD pill (v17)
    {
    public:
        void paint (juce::Graphics&) override;
    };

    void openSettingsFlyout();

    // ---- presets (Phase 5, spec §10; v17 header browser) --------------------------
    void refreshPresetBox();
    void loadPresetAtIndex (int index);
    void stepPreset (int delta);
    void openSavePresetDialog();

    // ---- pages / selection / overlay -------------------------------------------
    void setPage (int newPage); // 0 = MAIN, 1 = FX (closes the overlay, v17)
    int selectedChop();
    void setSelectedChop (int chop);
    void selectionChanged(); // syncs FX chip / overlay / grid chip
    void openChopEditor (int chop);
    void closeChopEditor();

    static constexpr int designWidth = 1100;
    static constexpr int designHeight = 720;

    ChoplineAudioProcessor& processorRef;
    ui::ChoplineLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 700 };

    juce::Component canvas; // fixed 1100x720, scaled by transform
    int page = 0;           // UI-only (v17 pages.persistence)

    // Header.
    juce::Label wordmark;
    juce::TextButton presetPrev { juce::CharPointer_UTF8 ("\xe2\x80\xb9") };  // ‹
    juce::TextButton presetNext { juce::CharPointer_UTF8 ("\xe2\x80\xba") };  // ›
    juce::ComboBox presetBox;
    MidiActivityDot midiDot;
    PageSeg pageSeg;
    GearButton gearButton;
    HoldBadge holdBadge; // visible on both pages while BR holds (v17 br_badge)

    // MAIN page.
    ui::SamplerStrip samplerStrip;
    ui::GridStrip gridStrip;
    ui::BeatRepeatPanel beatRepeatPanel; // Phase 4 (v17 br card)
    ui::MasterPanel masterPanel;         // Phase 4 (v17 master card)

    // FX page (CHOP column + Phase 4 MASTER column).
    ui::FxPage fxPage;

    // Chop detail overlay (topmost, v17 overlay).
    ui::ChopEditorOverlay chopOverlay;

    // Phase 5: presets + drag-out (declared before the VBlank so the lambda
    // can safely touch them).
    state::PresetManager presetManager;
    int currentPresetIndex = -1;
    PatternDragExporter dragExporter;

    juce::VBlankAttachment vblank;
    int gcCountdown = 0;
    bool momentaryHold = false;    // H key state (mirrors engine.setMomentaryHold)
    bool previewHeld = false;      // Space key state (Phase 5 transport preview)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoplineAudioProcessorEditor)
};

} // namespace chopline
