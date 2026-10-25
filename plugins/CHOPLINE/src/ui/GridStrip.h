// CHOPLINE — GridStrip (spec §5, §9; locked v17 mockup, strip_b)
// Strip B: the grid sequencer card. Grid row (compact note-length cycle
// button, pattern-length combo, bar-page seg, SWING knob) over the boxes
// display (v17 cards: white hairline boxes radius 11 on an #efeff1 tray,
// filled = chop tint + index chip + mini waveform, selected = blue outline,
// playing = pressed grey, blue 1.5 px playhead hairline). The per-step lane
// strip below stays a placeholder card until Phase 5.
//
// Interactions (spec §5 + v17): drop chops dragged from Strip A (a
// multi-selection fills consecutive cells in order); with a chop selected,
// click an empty cell to paint it and drag across cells to paint a run; drag
// an event's right edge to span steps; drag to move; Alt-drag duplicates;
// Delete removes; double-click selects the chop in Strip A; Cmd/Ctrl-Z and
// Shift-Cmd-Z undo/redo through the pattern model's juce::UndoManager.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/Engine.h"
#include "CompactKnob.h"
#include "LookAndFeel.h"

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

    /// ~60 fps tick from the editor's VBlankAttachment (playhead).
    void animationTick();

    void paint (juce::Graphics&) override;
    void resized() override;

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

        std::vector<Cell> computeCells() const;
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
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void syncControlsFromModel();
    int numPages() const;
    int clampedPage() const;

    engine::Engine& engine;
    state::PatternModel& model;
    int page = 0; // UI-only bar page (v17 bar_page)

    NoteLengthButton noteLenButton;
    juce::ComboBox lengthBox;
    BarPageSeg barPageSeg;
    CompactKnob swingKnob { "SWING" };
    std::unique_ptr<juce::SliderParameterAttachment> swingAttachment;
    BoxesDisplay boxes { *this };
    ChopChip chopChip;
    juce::TextButton chopMoreButton { juce::CharPointer_UTF8 ("\xe2\x80\xa6") }; // …

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GridStrip)
};

} // namespace chopline::ui
