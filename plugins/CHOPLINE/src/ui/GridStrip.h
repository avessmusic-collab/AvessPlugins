// CHOPLINE — GridStrip (spec §5, §9; locked v17 mockup, strip_b)
// Strip B: the grid sequencer card. Grid row (compact note-length cycle
// button, pattern-length combo, bar-page seg, SWING knob) over the boxes
// display (v17 cards: white hairline boxes radius 11 on an #efeff1 tray,
// filled = chop tint + index chip + mini waveform, selected = blue outline,
// playing = pressed grey, blue 1.5 px playhead hairline). Phase 5 replaces
// the lane placeholder with the v17 lane editor: the lane_tab seg
// (VEL·PITCH·PROB·RATCHET) over the step_lane display (1046×68) — per-step
// bars in the v17 display style; click/drag paints values across steps,
// double-click resets a step, only steps with events show editable bars, and
// a paint gesture is ONE undo transaction.
//
// Interactions (spec §5 + v17): drop chops dragged from Strip A (a
// multi-selection fills consecutive cells in order); with a chop selected,
// click an empty cell to paint it and drag across cells to paint a run; drag
// an event's right edge to span steps; drag to move; Alt-drag duplicates;
// Delete removes; LEFT/RIGHT arrows nudge the selected events by one step
// (Phase 5); double-click selects the chop in Strip A; Cmd/Ctrl-Z and
// Shift-Cmd-Z undo/redo through the pattern model's juce::UndoManager.
// Right-click on the boxes tray opens the Phase 5 GENERATORS menu (seeded
// randomize / shift / reverse / copy–paste / copy-to-slot — spec §5; lean,
// no new chrome per the locked v17).
//
// Drag-out (Phase 5, spec §10): dragging from the EMPTY grid-header area
// (the least-chrome home — between the bar seg and the SWING knob) exports
// the current pattern as a MIDI file; Alt-drag renders the pattern offline
// (background thread, subtle progress hairline) and drags the WAV out once
// it is ready. The editor wires the callbacks.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/Engine.h"
#include "CompactKnob.h"
#include "LookAndFeel.h"
#include "SegControl.h"

#include <set>
#include <vector>

namespace chopline::ui
{

class GridStrip final : public juce::Component,
                        private juce::ChangeListener
{
public:
    GridStrip (engine::Engine& engineToUse, juce::AudioProcessorValueTreeState& apvts);
    ~GridStrip() override;

    /// Wired by the editor: Strip A's chop selection (ascending order).
    std::function<std::vector<int>()> getSelectedChops;
    /// Wired by the editor: double-clicking a filled box selects that chop.
    std::function<void (int)> onSelectChop;
    /// Wired by the editor: the edited chop (v17 chop_chip) and the chop
    /// editor overlay ('…' button / double-click a filled box).
    std::function<int()> getEditChop;
    std::function<void()> onOpenChopEditor;

    // ---- Phase 5 drag-out (wired by the editor; see header comment) --------------
    /// Writes the current pattern as a temp .mid and returns it ({} on failure).
    std::function<juce::File()> createMidiDragFile;
    /// Starts (or restarts) the background audio render of the pattern.
    std::function<void()> startAudioRender;
    /// The finished render ({} while rendering / invalid).
    std::function<juce::File()> getRenderedAudioFile;
    /// <0 idle, 0..1 while rendering.
    std::function<float()> getAudioRenderProgress;

    /// ~60 fps tick from the editor's VBlankAttachment (playhead).
    void animationTick();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override; // header drag-out zone
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    // ---- compact note-length button (v17 note_length_button) --------------------
    class NoteLengthButton final : public juce::Component,
                                   public juce::SettableTooltipClient,
                                   private juce::Timer
    {
    public:
        std::function<engine::NoteLength()> getValue;
        std::function<void (engine::NoteLength)> setValue;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        void showMenu();
        void cycle();

        bool menuShownForThisPress = false;
        static constexpr int caretZoneWidth = 14; // v17: right 14 px opens the menu
        static constexpr int holdMenuMs = 380;
    };

    // ---- chop chip readout (v17 chop_chip: swatch + "CHOP nn") ---------------------
    class ChopChip final : public juce::Component
    {
    public:
        std::function<int()> getChop;
        void paint (juce::Graphics&) override;
    };

    // ---- bar page segment (visible when pattern length > 1 bar) -------------------
    class BarPageSeg final : public juce::Component
    {
    public:
        std::function<int()> getNumPages, getPage;
        std::function<void (int)> setPage;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
    };

    // ---- boxes display (v17 step_boxes, 1046x190) -----------------------------------
    class BoxesDisplay final : public juce::Component,
                               public juce::DragAndDropTarget
    {
    public:
        explicit BoxesDisplay (GridStrip& ownerToUse) : owner (ownerToUse)
        {
            setWantsKeyboardFocus (true);
        }

        void paint (juce::Graphics&) override;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        bool keyPressed (const juce::KeyPress&) override;

        // Chop drags from Strip A.
        bool isInterestedInDragSource (const SourceDetails&) override;
        void itemDragEnter (const SourceDetails&) override;
        void itemDragMove (const SourceDetails&) override;
        void itemDragExit (const SourceDetails&) override;
        void itemDropped (const SourceDetails&) override;

        std::set<int> selectedEventIds; // event ids (stable across undo)
        float lastPlayheadBar = -1.0f;  // page-local playhead, −1 = off
        bool brHold = false;            // playhead turns red while BR holds (v17)

    private:
        struct Cell
        {
            double startBar = 0.0, endBar = 0.0; // page-local, 0..1
            juce::Rectangle<float> rect;
            int row = 0;
        };
        struct Piece // one on-screen rectangle of an event (spans may split rows)
        {
            int eventId = -1;
            int chop = 0;
            juce::Rectangle<float> rect;
            bool isLastPiece = false; // right edge = span handle
        };

        const std::vector<Cell>& computeCells() const; // cached (Phase 5 perf)
        std::vector<Piece> computePieces (const std::vector<Cell>& cells) const;
        std::vector<int> getSelectedChopsSafe() const; // Strip A selection, valid chops only
        int cellIndexAt (juce::Point<float>, const std::vector<Cell>&) const; // −1 none
        const Piece* pieceAt (juce::Point<float>, const std::vector<Piece>&) const;
        double pageStartBar() const;
        void paintCellsFromDrag (const juce::StringArray& chopList, int firstCell);
        void drawMiniWaveform (juce::Graphics&, juce::Rectangle<float>, int chop) const;

        GridStrip& owner;

        enum class Gesture { none, paint, move, duplicate, resize };
        Gesture gesture = Gesture::none;
        int gestureEventId = -1;
        int gestureChop = 0;
        int hoverCell = -1;     // move/duplicate/drop target highlight
        int dropTargetCell = -1;
        bool movedSinceDown = false;
        mutable std::vector<Piece> pieceCache; // rebuilt per paint, for cursor hits

        // Cell geometry cache (Phase 5 polish): rebuilt only when the bounds
        // or the note length change.
        mutable std::vector<Cell> cellCache;
        mutable int cacheW = -1, cacheH = -1, cacheNoteLen = -1;
    };

    // ---- per-step lane editor (Phase 5; v17 lane_tab + step_lane) ------------------
    class LaneDisplay final : public juce::Component,
                              public juce::SettableTooltipClient
    {
    public:
        explicit LaneDisplay (GridStrip& ownerToUse) : owner (ownerToUse) {}

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

    private:
        struct Bar
        {
            int eventId = -1;
            juce::Rectangle<float> column; // full-height event column
            state::PatternModel::EventView view; // captured once per paint
        };
        std::vector<Bar> computeBars() const;
        juce::Rectangle<float> valueArea() const;
        void applyValueAt (const juce::MouseEvent&);
        juce::String formatValue (const state::PatternModel::EventView&) const;

        GridStrip& owner;
        bool gestureActive = false;
        int hoverEventId = -1;
        int dragEventId = -1; // the bar being edited (value pill)
    };

    void showGeneratorMenu();

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void syncControlsFromModel();
    int numPages() const;
    int clampedPage() const;

    engine::Engine& engine;
    state::PatternModel& model;
    int page = 0; // UI-only bar page (v17 bar_page)
    int lane = 0; // UI-only lane tab: 0 VEL, 1 PITCH, 2 PROB, 3 RATCHET

    NoteLengthButton noteLenButton;
    juce::ComboBox lengthBox;
    BarPageSeg barPageSeg;
    CompactKnob swingKnob { "SWING" };
    std::unique_ptr<juce::SliderParameterAttachment> swingAttachment;
    BoxesDisplay boxes { *this };
    SegControl laneTab;              // v17 lane_tab (VEL PITCH PROB RATCHET)
    LaneDisplay laneDisplay { *this }; // v17 step_lane (1046×68)
    ChopChip chopChip;
    juce::TextButton chopMoreButton { juce::CharPointer_UTF8 ("\xe2\x80\xa6") }; // …

    // Drag-out gesture state (Phase 5).
    bool dragArmed = false;
    bool dragWantsAudio = false;
    bool externalDragStarted = false;
    float lastRenderProgress = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GridStrip)
};

} // namespace chopline::ui
