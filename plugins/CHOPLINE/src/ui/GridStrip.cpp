// CHOPLINE — GridStrip implementation. See header.

#include "GridStrip.h"

#include "../state/Parameters.h"
#include "WaveformDisplay.h"

#include <cmath>

namespace chopline::ui
{

using LnF = ChoplineLookAndFeel;

namespace
{
    constexpr double barEps = 1.0e-6;
    const juce::Colour trayColour { 0xffefeff1 };
    const juce::Colour pressedGrey { 0xffededf0 };
} // namespace

// ============================= NoteLengthButton =====================================

void GridStrip::NoteLengthButton::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (LnF::cell);
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b, 7.0f, 1.0f);

    const auto value = getValue != nullptr ? getValue() : engine::NoteLength::sixteenth;
    g.setColour (LnF::ink);
    g.setFont (LnF::monoFont (10.0f));
    g.drawText (engine::noteLengthLabel (value),
                getLocalBounds().withTrimmedRight (caretZoneWidth).withTrimmedLeft (4),
                juce::Justification::centred, false);

    // Caret zone (right 14 px).
    const auto cx = static_cast<float> (getWidth() - 9);
    const auto cy = static_cast<float> (getHeight()) * 0.5f;
    juce::Path caret;
    caret.startNewSubPath (cx - 3.0f, cy - 1.8f);
    caret.lineTo (cx, cy + 1.8f);
    caret.lineTo (cx + 3.0f, cy - 1.8f);
    g.setColour (LnF::textDim);
    g.strokePath (caret, juce::PathStrokeType (1.2f));
}

void GridStrip::NoteLengthButton::mouseDown (const juce::MouseEvent& e)
{
    menuShownForThisPress = false;
    if (e.mods.isPopupMenu() || e.position.x >= static_cast<float> (getWidth() - caretZoneWidth))
    {
        menuShownForThisPress = true;
        showMenu();
        return;
    }
    startTimer (holdMenuMs); // click-and-hold opens the menu (v17: 380 ms)
}

void GridStrip::NoteLengthButton::mouseUp (const juce::MouseEvent& e)
{
    stopTimer();
    if (! menuShownForThisPress && ! e.mods.isPopupMenu()
        && getLocalBounds().toFloat().contains (e.position))
        cycle();
}

void GridStrip::NoteLengthButton::timerCallback()
{
    stopTimer();
    menuShownForThisPress = true;
    showMenu();
}

void GridStrip::NoteLengthButton::cycle()
{
    if (getValue == nullptr || setValue == nullptr)
        return;
    // Cycle the base length 1/4 → 1/8 → 1/16 (wrap), keeping the T/D variant.
    const auto idx = static_cast<int> (getValue());
    const auto base = idx / 3;
    const auto variant = idx % 3;
    setValue (static_cast<engine::NoteLength> (((base + 1) % 3) * 3 + variant));
    repaint();
}

void GridStrip::NoteLengthButton::showMenu()
{
    if (getValue == nullptr)
        return;
    juce::PopupMenu menu;
    const auto current = static_cast<int> (getValue());
    for (int i = 0; i < engine::numNoteLengths; ++i)
        menu.addItem (i + 1, engine::noteLengthLabel (static_cast<engine::NoteLength> (i)),
                      true, i == current);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safe = juce::Component::SafePointer<NoteLengthButton> (this)] (int result)
                        {
                            if (safe != nullptr && result > 0 && safe->setValue != nullptr)
                            {
                                safe->setValue (static_cast<engine::NoteLength> (result - 1));
                                safe->repaint();
                            }
                        });
}

// ================================ ChopChip ===========================================

void GridStrip::ChopChip::paint (juce::Graphics& g)
{
    const auto chop = getChop != nullptr ? getChop() : 0;
    const auto shade = LnF::chopShade (chop);

    // Swatch with the blue selection ring (v17 chop_chip.blue_ring_swatch).
    const auto swatch = juce::Rectangle<float> (2.0f,
        static_cast<float> (getHeight()) * 0.5f - 5.0f, 10.0f, 10.0f);
    g.setColour (shade);
    g.fillRoundedRectangle (swatch, 2.5f);
    g.setColour (LnF::accent);
    g.drawRoundedRectangle (swatch.expanded (1.5f), 3.5f, 1.0f);

    g.setColour (LnF::label);
    g.setFont (LnF::labelFont (8.5f));
    g.drawText ("CHOP " + juce::String (chop + 1), getLocalBounds().withTrimmedLeft (20),
                juce::Justification::centredLeft, false);
}

// ================================ BarPageSeg =========================================

void GridStrip::BarPageSeg::paint (juce::Graphics& g)
{
    const auto n = getNumPages != nullptr ? getNumPages() : 1;
    const auto active = getPage != nullptr ? getPage() : 0;

    auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (LnF::cell);
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b, 7.0f, 1.0f);

    const auto segW = b.getWidth() / static_cast<float> (juce::jmax (1, n));
    for (int i = 0; i < n; ++i)
    {
        auto seg = juce::Rectangle<float> (b.getX() + segW * static_cast<float> (i),
                                           b.getY(), segW, b.getHeight()).reduced (2.0f);
        if (i == active)
        {
            g.setColour (LnF::ink);
            g.fillRoundedRectangle (seg, 5.0f);
        }
        g.setFont (LnF::labelFont (8.0f));
        g.setColour (i == active ? juce::Colours::white : LnF::label);
        g.drawText (juce::String (i + 1), seg.toNearestInt(),
                    juce::Justification::centred, false);
    }
}

void GridStrip::BarPageSeg::mouseDown (const juce::MouseEvent& e)
{
    const auto n = getNumPages != nullptr ? getNumPages() : 1;
    if (n <= 0 || getWidth() <= 0 || setPage == nullptr)
        return;
    const auto seg = juce::jlimit (0, n - 1,
                                   static_cast<int> (e.position.x
                                                     / (static_cast<float> (getWidth())
                                                        / static_cast<float> (n))));
    setPage (seg);
    repaint();
}

// ================================ BoxesDisplay ========================================

double GridStrip::BoxesDisplay::pageStartBar() const
{
    return static_cast<double> (owner.clampedPage());
}

std::vector<GridStrip::BoxesDisplay::Cell> GridStrip::BoxesDisplay::computeCells() const
{
    std::vector<Cell> cells;
    const auto slot = owner.model.getActiveSlot();
    const auto noteLen = owner.model.getNoteLength (slot);
    const auto stepLen = engine::noteLengthBars (noteLen);
    const auto n = engine::noteLengthStepsPerBar (noteLen);
    if (n <= 0 || stepLen <= 0.0)
        return cells;

    const auto rows = n > 12 ? 2 : 1; // v17: max 24 boxes = 12 columns x 2 rows
    const auto row0Count = rows == 2 ? (n + 1) / 2 : n;

    const auto area = getLocalBounds().toFloat().reduced (7.0f);
    const auto gapY = 6.0f;
    const auto rowH = (area.getHeight() - gapY * static_cast<float> (rows - 1))
                      / static_cast<float> (rows);

    cells.reserve (static_cast<size_t> (n));
    for (int r = 0; r < rows; ++r)
    {
        const auto firstStep = r == 0 ? 0 : row0Count;
        const auto lastStep = r == 0 ? row0Count : n;
        const auto rowStart = static_cast<double> (firstStep) * stepLen;
        const auto rowEnd = lastStep == n ? 1.0 : static_cast<double> (lastStep) * stepLen;
        if (rowEnd <= rowStart)
            continue;

        const auto rowY = area.getY() + static_cast<float> (r) * (rowH + gapY);
        for (int i = firstStep; i < lastStep; ++i)
        {
            Cell c;
            c.startBar = static_cast<double> (i) * stepLen;
            c.endBar = juce::jmin (1.0, static_cast<double> (i + 1) * stepLen);
            c.row = r;
            const auto x0 = area.getX()
                + static_cast<float> ((c.startBar - rowStart) / (rowEnd - rowStart))
                      * area.getWidth();
            const auto x1 = area.getX()
                + static_cast<float> ((c.endBar - rowStart) / (rowEnd - rowStart))
                      * area.getWidth();
            c.rect = juce::Rectangle<float> (x0 + 2.0f, rowY, juce::jmax (2.0f, x1 - x0 - 4.0f), rowH);
            cells.push_back (c);
        }
    }
    return cells;
}

std::vector<GridStrip::BoxesDisplay::Piece>
GridStrip::BoxesDisplay::computePieces (const std::vector<Cell>& cells) const
{
    std::vector<Piece> pieces;
    if (cells.empty())
        return pieces;

    const auto slot = owner.model.getActiveSlot();
    const auto pageStart = pageStartBar();
    const auto numEvents = owner.model.getNumEvents (slot);

    for (int e = 0; e < numEvents; ++e)
    {
        const auto ev = owner.model.getEvent (slot, e);
        const auto local = ev.startBar - pageStart;
        if (local < -barEps || local >= 1.0 - barEps)
            continue; // other page (spans across a page boundary draw on their own page)

        // Start cell = the cell containing the (possibly off-grid) event start.
        int startCell = -1;
        for (size_t i = 0; i < cells.size(); ++i)
            if (local >= cells[i].startBar - barEps && local < cells[i].endBar - barEps)
            {
                startCell = static_cast<int> (i);
                break;
            }
        if (startCell < 0)
            continue;

        const auto evEnd = local + juce::jmax (ev.lenBars, barEps);
        auto endCell = startCell;
        for (size_t i = static_cast<size_t> (startCell); i < cells.size(); ++i)
            if (cells[i].startBar < evEnd - barEps)
                endCell = static_cast<int> (i);

        for (auto row = cells[static_cast<size_t> (startCell)].row;
             row <= cells[static_cast<size_t> (endCell)].row; ++row)
        {
            int first = -1, last = -1;
            for (int i = startCell; i <= endCell; ++i)
            {
                if (cells[static_cast<size_t> (i)].row != row)
                    continue;
                if (first < 0)
                    first = i;
                last = i;
            }
            if (first < 0)
                continue;
            Piece piece;
            piece.eventId = ev.id;
            piece.chop = ev.chop;
            piece.rect = cells[static_cast<size_t> (first)].rect
                             .getUnion (cells[static_cast<size_t> (last)].rect);
            piece.isLastPiece = last == endCell;
            pieces.push_back (piece);
        }
    }
    return pieces;
}

std::vector<int> GridStrip::BoxesDisplay::getSelectedChopsSafe() const
{
    if (owner.getSelectedChops == nullptr)
        return {};
    const auto numChops = owner.engine.getNumChops();
    std::vector<int> valid;
    for (auto c : owner.getSelectedChops())
        if (c >= 0 && c < numChops)
            valid.push_back (c);
    return valid;
}

int GridStrip::BoxesDisplay::cellIndexAt (juce::Point<float> p,
                                          const std::vector<Cell>& cells) const
{
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i].rect.expanded (2.0f, 3.0f).contains (p))
            return static_cast<int> (i);
    return -1;
}

const GridStrip::BoxesDisplay::Piece*
GridStrip::BoxesDisplay::pieceAt (juce::Point<float> p, const std::vector<Piece>& pieces) const
{
    for (const auto& piece : pieces)
        if (piece.rect.contains (p))
            return &piece;
    return nullptr;
}

void GridStrip::BoxesDisplay::drawMiniWaveform (juce::Graphics& g, juce::Rectangle<float> r,
                                                int chop) const
{
    const auto sample = owner.engine.currentSample();
    const auto& markers = owner.engine.getMarkers();
    if (sample == nullptr || sample->numFrames() <= 0 || markers.empty()
        || chop < 0 || chop >= static_cast<int> (markers.size()))
        return;

    const auto startSec = markers[static_cast<size_t> (chop)].timeSec;
    const auto endSec = chop + 1 < static_cast<int> (markers.size())
        ? markers[static_cast<size_t> (chop) + 1].timeSec
        : sample->lengthSeconds();
    if (endSec <= startSec)
        return;

    // 12 px hairline grid (v17 box mini-waveform window).
    g.setColour (LnF::gridLine());
    for (auto gx = r.getX() + 12.0f; gx < r.getRight(); gx += 12.0f)
        g.drawVerticalLine (static_cast<int> (gx), r.getY(), r.getBottom());
    for (auto gy = r.getY() + 12.0f; gy < r.getBottom(); gy += 12.0f)
        g.drawHorizontalLine (static_cast<int> (gy), r.getX(), r.getRight());

    const auto rate = sample->sessionRate;
    const auto total = sample->numFrames();
    const auto midY = r.getCentreY();
    const auto halfH = r.getHeight() * 0.38f;
    const auto* raw = sample->audio.getReadPointer (0);
    const auto& peaks = sample->peaksFine;

    g.setColour (LnF::ink.withAlpha (0.8f));
    const auto cols = juce::jmax (1, static_cast<int> (r.getWidth() / 2.0f));
    for (int col = 0; col < cols; ++col)
    {
        const auto t0 = startSec + (endSec - startSec) * col / cols;
        const auto t1 = startSec + (endSec - startSec) * (col + 1) / cols;
        const auto s0 = juce::jlimit<juce::int64> (0, total, static_cast<juce::int64> (t0 * rate));
        const auto s1 = juce::jlimit<juce::int64> (0, total, static_cast<juce::int64> (t1 * rate) + 1);
        if (s1 <= s0)
            continue;

        float mn = 1.0e9f, mx = -1.0e9f;
        if (peaks.bucketSize > 0 && ! peaks.mins.empty())
        {
            const auto b0 = juce::jmin<juce::int64> (static_cast<juce::int64> (peaks.mins.size()) - 1,
                                                     s0 / peaks.bucketSize);
            const auto b1 = juce::jmin<juce::int64> (static_cast<juce::int64> (peaks.mins.size()) - 1,
                                                     (s1 - 1) / peaks.bucketSize);
            for (auto b = b0; b <= b1; ++b)
            {
                mn = juce::jmin (mn, peaks.mins[static_cast<size_t> (b)]);
                mx = juce::jmax (mx, peaks.maxs[static_cast<size_t> (b)]);
            }
        }
        else
        {
            const auto stride = juce::jmax<juce::int64> (1, (s1 - s0) / 32);
            for (auto i = s0; i < s1; i += stride)
            {
                mn = juce::jmin (mn, raw[i]);
                mx = juce::jmax (mx, raw[i]);
            }
        }
        if (mn > mx)
            continue;

        const auto x = r.getX() + r.getWidth() * (static_cast<float> (col) + 0.5f)
                                      / static_cast<float> (cols);
        const auto yTop = midY - juce::jlimit (-1.0f, 1.0f, mx) * halfH;
        const auto yBot = midY - juce::jlimit (-1.0f, 1.0f, mn) * halfH;
        g.drawLine (x, yTop, x, juce::jmax (yBot, yTop + 1.0f), 1.0f);
    }
}

void GridStrip::BoxesDisplay::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();

    // Tray (v17 boxes.tray).
    g.setColour (trayColour);
    g.fillRoundedRectangle (area, 10.0f);
    g.setColour (LnF::gridLine());
    g.drawRoundedRectangle (area.reduced (0.5f), 10.0f, 1.0f);

    const auto cells = computeCells();
    const auto pieces = computePieces (cells);
    pieceCache = pieces;

    // Empty cell cards (white hairline, radius 11, 1 px soft shadow).
    for (const auto& c : cells)
    {
        g.setColour (juce::Colours::black.withAlpha (0.04f));
        g.fillRoundedRectangle (c.rect.translated (0.0f, 1.0f), 11.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (c.rect, 11.0f);
        g.setColour (LnF::hairline());
        g.drawRoundedRectangle (c.rect.reduced (0.5f), 11.0f, 1.0f);
    }

    // Playhead position (page-local bars) for the "playing" box state.
    const auto playPos = static_cast<double> (lastPlayheadBar);
    const auto slot = owner.model.getActiveSlot();

    // Event pieces.
    for (const auto& piece : pieces)
    {
        const auto ev = owner.model.getEventById (slot, piece.eventId);
        const auto shade = LnF::chopShade (piece.chop);
        const bool selected = selectedEventIds.count (piece.eventId) > 0;
        const bool playing = playPos >= 0.0
            && playPos >= ev.startBar - pageStartBar()
            && playPos < ev.startBar - pageStartBar() + juce::jmax (ev.lenBars, barEps);

        g.setColour (playing ? pressedGrey : juce::Colours::white);
        g.fillRoundedRectangle (piece.rect, 11.0f);
        g.setColour (shade.withAlpha (0.12f)); // faint grey tint of the chop shade
        g.fillRoundedRectangle (piece.rect, 11.0f);

        {
            juce::Graphics::ScopedSaveState save (g);
            juce::Path clip;
            clip.addRoundedRectangle (piece.rect, 11.0f);
            g.reduceClipRegion (clip);
            drawMiniWaveform (g, piece.rect.reduced (4.0f, 6.0f), piece.chop);
        }

        g.setColour (LnF::hairline());
        g.drawRoundedRectangle (piece.rect.reduced (0.5f), 11.0f, 1.0f);
        if (playing) // inset hairline (v17 "pressed grey + inset hairline")
            g.drawRoundedRectangle (piece.rect.reduced (2.5f), 9.0f, 1.0f);

        if (selected)
        {
            g.setColour (LnF::accent);
            g.drawRoundedRectangle (piece.rect.reduced (0.75f), 11.0f, 1.5f);
        }

        // Index chip (1-based chop number).
        if (piece.rect.getWidth() > 20.0f)
        {
            const auto chip = juce::Rectangle<float> (piece.rect.getX() + 5.0f,
                                                      piece.rect.getY() + 5.0f, 16.0f, 12.0f);
            g.setColour (shade);
            g.fillRoundedRectangle (chip, 3.0f);
            if (selected)
            {
                g.setColour (LnF::accent);
                g.drawRoundedRectangle (chip.expanded (1.0f), 4.0f, 1.0f);
            }
            g.setColour (shade.getPerceivedBrightness() > 0.6f ? LnF::ink : juce::Colours::white);
            g.setFont (LnF::monoFont (8.0f));
            g.drawText (juce::String (piece.chop + 1), chip, juce::Justification::centred, false);
        }

        // Span handle hint on the last piece.
        if (piece.isLastPiece && piece.rect.getWidth() > 16.0f)
        {
            g.setColour (LnF::hairline());
            const auto hx = piece.rect.getRight() - 4.0f;
            g.drawLine (hx, piece.rect.getY() + 6.0f, hx, piece.rect.getBottom() - 6.0f, 1.0f);
        }
    }

    // Move / duplicate / drop target highlight.
    const auto target = dropTargetCell >= 0 ? dropTargetCell
                                            : (gesture == Gesture::move || gesture == Gesture::duplicate)
                                                  && movedSinceDown ? hoverCell : -1;
    if (target >= 0 && target < static_cast<int> (cells.size()))
    {
        g.setColour (LnF::accent.withAlpha (0.55f));
        g.drawRoundedRectangle (cells[static_cast<size_t> (target)].rect.reduced (0.75f),
                                11.0f, 1.5f);
    }

    // Playhead: 1.5 px blue hairline moving through the boxes (v17).
    if (playPos >= 0.0)
    {
        for (const auto& c : cells)
        {
            if (playPos >= c.startBar && playPos < c.endBar)
            {
                const auto frac = (playPos - c.startBar) / juce::jmax (barEps, c.endBar - c.startBar);
                const auto x = c.rect.getX() + static_cast<float> (frac) * c.rect.getWidth();
                g.setColour (LnF::accent);
                g.drawLine (x, c.rect.getY() - 3.0f, x, c.rect.getBottom() + 3.0f, 1.5f);
                break;
            }
        }
    }
}

// ---- boxes interaction -----------------------------------------------------------------

void GridStrip::BoxesDisplay::mouseMove (const juce::MouseEvent& e)
{
    const auto* piece = pieceAt (e.position, pieceCache);
    const bool onSpanHandle = piece != nullptr && piece->isLastPiece
                              && e.position.x > piece->rect.getRight() - 7.0f;
    setMouseCursor (onSpanHandle ? juce::MouseCursor::LeftRightResizeCursor
                                 : juce::MouseCursor::NormalCursor);
}

void GridStrip::BoxesDisplay::mouseExit (const juce::MouseEvent&)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void GridStrip::BoxesDisplay::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    movedSinceDown = false;
    hoverCell = -1;
    gesture = Gesture::none;
    gestureEventId = -1;

    auto& pm = owner.model;
    const auto slot = pm.getActiveSlot();
    const auto cells = computeCells();
    const auto pieces = computePieces (cells);

    if (const auto* piece = pieceAt (e.position, pieces))
    {
        if (e.mods.isShiftDown())
        {
            if (selectedEventIds.count (piece->eventId) > 0)
                selectedEventIds.erase (piece->eventId);
            else
                selectedEventIds.insert (piece->eventId);
        }
        else if (selectedEventIds.count (piece->eventId) == 0)
        {
            selectedEventIds.clear();
            selectedEventIds.insert (piece->eventId);
        }

        gestureEventId = piece->eventId;
        gestureChop = piece->chop;

        const bool onSpanHandle = piece->isLastPiece
                                  && e.position.x > piece->rect.getRight() - 7.0f;
        if (onSpanHandle && ! e.mods.isAltDown())
        {
            gesture = Gesture::resize;
            pm.beginTransaction ("Span event");
        }
        else
        {
            gesture = e.mods.isAltDown() ? Gesture::duplicate : Gesture::move;
        }
        repaint();
        return;
    }

    // Empty cell: paint the chop selected in Strip A (spec §5).
    const auto cellIdx = cellIndexAt (e.position, cells);
    if (cellIdx < 0)
    {
        selectedEventIds.clear();
        repaint();
        return;
    }

    const auto chops = getSelectedChopsSafe();
    if (chops.empty())
    {
        selectedEventIds.clear();
        repaint();
        return;
    }

    gesture = Gesture::paint;
    gestureChop = chops.front();
    pm.beginTransaction ("Paint steps");

    const auto& cell = cells[static_cast<size_t> (cellIdx)];
    const auto id = pm.addEvent (slot, pageStartBar() + cell.startBar,
                                    cell.endBar - cell.startBar, gestureChop);
    selectedEventIds.clear();
    if (id >= 0)
        selectedEventIds.insert (id);
    repaint();
}

void GridStrip::BoxesDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (gesture == Gesture::none)
        return;
    if (e.getDistanceFromDragStart() > 3)
        movedSinceDown = true;

    auto& pm = owner.model;
    const auto slot = pm.getActiveSlot();
    const auto cells = computeCells();
    const auto cellIdx = cellIndexAt (e.position, cells);
    if (cellIdx < 0)
        return;
    const auto& cell = cells[static_cast<size_t> (cellIdx)];

    if (gesture == Gesture::paint)
    {
        const auto start = pageStartBar() + cell.startBar;
        if (pm.findEventAt (slot, start + barEps) < 0) // cell still empty
        {
            const auto id = pm.addEvent (slot, start, cell.endBar - cell.startBar, gestureChop);
            if (id >= 0)
                selectedEventIds.insert (id);
            repaint();
        }
        return;
    }

    if (gesture == Gesture::resize)
    {
        const auto ev = pm.getEventById (slot, gestureEventId);
        if (ev.id < 0)
            return;
        // Span from the event start to the end of the cell under the mouse
        // (never below one step).
        const auto newEnd = pageStartBar() + cell.endBar;
        const auto oneStep = cell.endBar - cell.startBar;
        const auto newLen = juce::jmax (oneStep, newEnd - ev.startBar);
        pm.setEventLength (slot, gestureEventId, newLen);
        repaint();
        return;
    }

    // move / duplicate: track the target cell.
    if (cellIdx != hoverCell)
    {
        hoverCell = cellIdx;
        repaint();
    }
}

void GridStrip::BoxesDisplay::mouseUp (const juce::MouseEvent&)
{
    auto& pm = owner.model;
    const auto slot = pm.getActiveSlot();

    if ((gesture == Gesture::move || gesture == Gesture::duplicate)
        && movedSinceDown && hoverCell >= 0)
    {
        const auto cells = computeCells();
        if (hoverCell < static_cast<int> (cells.size()))
        {
            const auto& cell = cells[static_cast<size_t> (hoverCell)];
            const auto newStart = pageStartBar() + cell.startBar;

            pm.beginTransaction (gesture == Gesture::move ? "Move event" : "Duplicate event");

            // Replace whatever event starts in the target cell.
            const auto occupant = pm.findEventStartingIn (slot, newStart - barEps,
                                                             pageStartBar() + cell.endBar - barEps);
            if (occupant >= 0 && occupant != gestureEventId)
                pm.removeEvent (slot, occupant);

            if (gesture == Gesture::move)
            {
                pm.moveEvent (slot, gestureEventId, newStart);
            }
            else
            {
                const auto id = pm.duplicateEvent (slot, gestureEventId, newStart);
                if (id >= 0)
                {
                    selectedEventIds.clear();
                    selectedEventIds.insert (id);
                }
            }
        }
    }

    gesture = Gesture::none;
    gestureEventId = -1;
    hoverCell = -1;
    movedSinceDown = false;
    repaint();
}

void GridStrip::BoxesDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto* piece = pieceAt (e.position, pieceCache))
    {
        if (owner.onSelectChop != nullptr)
            owner.onSelectChop (piece->chop); // selects the chop in Strip A (spec §5)
        if (owner.onOpenChopEditor != nullptr)
            owner.onOpenChopEditor(); // and opens the chop editor (v17)
    }
}

bool GridStrip::BoxesDisplay::keyPressed (const juce::KeyPress& key)
{
    auto& pm = owner.model;

    if (key.isKeyCode (juce::KeyPress::deleteKey) || key.isKeyCode (juce::KeyPress::backspaceKey))
    {
        if (selectedEventIds.empty())
            return false;
        pm.beginTransaction ("Delete events");
        const auto slot = pm.getActiveSlot();
        for (auto id : selectedEventIds)
            pm.removeEvent (slot, id);
        selectedEventIds.clear();
        repaint();
        return true;
    }

    if (key.getModifiers().isCommandDown()
        && (key.isKeyCode ('z') || key.isKeyCode ('Z')))
    {
        if (key.getModifiers().isShiftDown())
            pm.getUndoManager().redo();
        else
            pm.getUndoManager().undo();
        repaint();
        return true;
    }

    return false;
}

// ---- drag-and-drop from Strip A -------------------------------------------------------

bool GridStrip::BoxesDisplay::isInterestedInDragSource (const SourceDetails& details)
{
    return details.description.toString().startsWith (WaveformDisplay::chopDragPrefix);
}

void GridStrip::BoxesDisplay::itemDragEnter (const SourceDetails& details)
{
    itemDragMove (details);
}

void GridStrip::BoxesDisplay::itemDragMove (const SourceDetails& details)
{
    const auto cells = computeCells();
    const auto idx = cellIndexAt (details.localPosition.toFloat(), cells);
    if (idx != dropTargetCell)
    {
        dropTargetCell = idx;
        repaint();
    }
}

void GridStrip::BoxesDisplay::itemDragExit (const SourceDetails&)
{
    dropTargetCell = -1;
    repaint();
}

void GridStrip::BoxesDisplay::itemDropped (const SourceDetails& details)
{
    const auto target = dropTargetCell;
    dropTargetCell = -1;

    auto list = details.description.toString()
                    .fromFirstOccurrenceOf (WaveformDisplay::chopDragPrefix, false, false);
    juce::StringArray chopStrings;
    chopStrings.addTokens (list, ",", {});
    if (target < 0 || chopStrings.isEmpty())
    {
        repaint();
        return;
    }
    paintCellsFromDrag (chopStrings, target);
}

void GridStrip::BoxesDisplay::paintCellsFromDrag (const juce::StringArray& chopList, int firstCell)
{
    auto& pm = owner.model;
    const auto slot = pm.getActiveSlot();
    const auto cells = computeCells();
    const auto numChops = owner.engine.getNumChops();

    pm.beginTransaction ("Drop chops");
    selectedEventIds.clear();

    // A multi-selection fills consecutive cells in order (spec §5).
    int cellIdx = firstCell;
    for (const auto& chopString : chopList)
    {
        if (cellIdx >= static_cast<int> (cells.size()))
            break;
        const auto chop = chopString.getIntValue();
        if (chop < 0 || chop >= numChops)
            continue;

        const auto& cell = cells[static_cast<size_t> (cellIdx)];
        const auto start = pageStartBar() + cell.startBar;

        const auto occupant = pm.findEventStartingIn (slot, start - barEps,
                                                         pageStartBar() + cell.endBar - barEps);
        if (occupant >= 0)
            pm.removeEvent (slot, occupant);

        const auto id = pm.addEvent (slot, start, cell.endBar - cell.startBar, chop);
        if (id >= 0)
            selectedEventIds.insert (id);
        ++cellIdx;
    }
    repaint();
}

// ================================== GridStrip ==========================================

GridStrip::GridStrip (engine::Engine& engineToUse, juce::AudioProcessorValueTreeState& apvts)
    : engine (engineToUse), model (engineToUse.getPatternModel())
{
    noteLenButton.getValue = [this] { return model.getNoteLength (model.getActiveSlot()); };
    noteLenButton.setValue = [this] (engine::NoteLength n)
    {
        model.setNoteLength (model.getActiveSlot(), n);
    };
    noteLenButton.setTooltip ("Note length");
    addAndMakeVisible (noteLenButton);

    lengthBox.addItem ("1 bar", 1);
    lengthBox.addItem ("2 bars", 2);
    lengthBox.addItem ("4 bars", 3);
    lengthBox.onChange = [this]
    {
        const int bars[] = { 1, 2, 4 };
        const auto sel = juce::jlimit (0, 2, lengthBox.getSelectedId() - 1);
        model.setPatternBars (model.getActiveSlot(), bars[sel]);
    };
    addAndMakeVisible (lengthBox);

    barPageSeg.getNumPages = [this] { return numPages(); };
    barPageSeg.getPage = [this] { return clampedPage(); };
    barPageSeg.setPage = [this] (int p)
    {
        page = p;
        boxes.repaint();
        barPageSeg.repaint();
    };
    addChildComponent (barPageSeg); // visible only when pattern length > 1 bar

    swingKnob.getSlider().setRange (0.0, 75.0, 0.1);
    swingKnob.formatValue = [] (double v)
    {
        return juce::String (static_cast<int> (std::lround (v))) + "%";
    };
    if (auto* param = apvts.getParameter (params::id::swing))
        swingAttachment = std::make_unique<juce::SliderParameterAttachment> (*param,
                                                                             swingKnob.getSlider());
    addAndMakeVisible (swingKnob);

    addAndMakeVisible (boxes);

    // Lane-row chop chip + '…' (v17 chop_chip / chop_more, beside the Phase 5
    // lanes placeholder).
    chopChip.getChop = [this] { return getEditChop != nullptr ? getEditChop() : 0; };
    addAndMakeVisible (chopChip);
    chopMoreButton.onClick = [this]
    {
        if (onOpenChopEditor != nullptr)
            onOpenChopEditor();
    };
    chopMoreButton.setTooltip ("Chop settings");
    addAndMakeVisible (chopMoreButton);

    syncControlsFromModel();
    engine.addChangeListener (this);
}

GridStrip::~GridStrip()
{
    engine.removeChangeListener (this);
}

int GridStrip::numPages() const
{
    return model.getPatternBars (model.getActiveSlot());
}

int GridStrip::clampedPage() const
{
    return juce::jlimit (0, juce::jmax (0, numPages() - 1), page);
}

void GridStrip::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncControlsFromModel();
    boxes.repaint();
}

void GridStrip::syncControlsFromModel()
{
    const auto bars = model.getPatternBars (model.getActiveSlot());
    lengthBox.setSelectedId (bars >= 4 ? 3 : (bars >= 2 ? 2 : 1), juce::dontSendNotification);
    page = clampedPage();
    barPageSeg.setVisible (bars > 1);
    barPageSeg.repaint();
    noteLenButton.repaint();
}

void GridStrip::animationTick()
{
    const auto pos = engine.getUiPatternPositionBars();
    float local = -1.0f;
    if (pos >= 0.0f)
    {
        const auto p = pos - static_cast<float> (clampedPage());
        if (p >= 0.0f && p < 1.0f)
            local = p;
    }
    if (! juce::exactlyEqual (local, boxes.lastPlayheadBar))
    {
        boxes.lastPlayheadBar = local;
        boxes.repaint();
    }
}

void GridStrip::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (LnF::paper);
    g.fillRoundedRectangle (b, 16.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b.reduced (0.5f), 16.0f, 1.0f);

    // Per-step lane strip area: Phase 5 placeholder (v17 lane_tab + step_lane).
    const auto lanes = juce::Rectangle<float> (12.0f, 266.0f, 1046.0f, 102.0f);
    g.setColour (LnF::display);
    g.fillRoundedRectangle (lanes, 10.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (lanes.reduced (0.5f), 10.0f, 1.0f);
    g.setColour (LnF::textTertiary);
    g.setFont (LnF::labelFont (8.5f));
    g.drawText (juce::String (juce::CharPointer_UTF8 ("LANES \xe2\x80\x94 PHASE 5")),
                lanes.toNearestInt(), juce::Justification::centred, false);
}

void GridStrip::resized()
{
    // v17 strip_b geometry relative to the card at (15, 206).
    noteLenButton.setBounds (12, 22, 64, 24);
    lengthBox.setBounds (88, 20, 100, 28);
    barPageSeg.setBounds (198, 20, 140, 28);
    swingKnob.setBounds (928, 14, 130, 40);
    boxes.setBounds (12, 64, 1046, 190);
    chopChip.setBounds (878, 268, 124, 20);      // v17 chop_chip (893,474 canvas)
    chopMoreButton.setBounds (1012, 268, 46, 20); // v17 chop_more (1027,474 canvas)
}

} // namespace chopline::ui
