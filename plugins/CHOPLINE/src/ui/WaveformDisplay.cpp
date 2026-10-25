// CHOPLINE — WaveformDisplay implementation. See header.

#include "WaveformDisplay.h"

#include <cmath>

namespace chopline::ui
{

namespace
{
    constexpr float cornerRadius = 10.0f;
    constexpr float gridStep = 22.0f;
    constexpr float markerHitZone = 4.0f;
    constexpr double minViewSeconds = 0.01;
} // namespace

WaveformDisplay::WaveformDisplay (engine::Engine& engineToUse)
    : engine (engineToUse)
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

// ---- model helpers --------------------------------------------------------------

double WaveformDisplay::sampleLengthSeconds() const
{
    const auto s = sample();
    return s != nullptr ? s->lengthSeconds() : 0.0;
}

std::vector<WaveformDisplay::ChopSpan> WaveformDisplay::chopSpans() const
{
    std::vector<ChopSpan> spans;
    const auto length = sampleLengthSeconds();
    const auto& markers = engine.getMarkers();
    if (length <= 0.0 || markers.empty())
        return spans;

    spans.reserve (markers.size());
    for (size_t i = 0; i < markers.size(); ++i)
        spans.push_back ({ markers[i].timeSec,
                           i + 1 < markers.size() ? markers[i + 1].timeSec : length });
    return spans;
}

void WaveformDisplay::modelChanged()
{
    const auto length = sampleLengthSeconds();
    if (length <= 0.0)
    {
        viewStart = 0.0;
        viewLen = 0.0;
        selection.clear();
    }
    else if (viewLen <= 0.0 || viewStart + viewLen > length + 1.0e-9)
    {
        viewStart = 0.0;
        viewLen = length;
    }

    const auto numChops = static_cast<int> (chopSpans().size());
    for (auto it = selection.begin(); it != selection.end();)
        it = *it >= numChops ? selection.erase (it) : std::next (it);

    repaint();
}

void WaveformDisplay::animationTick()
{
    // Signature of what is animating: voice positions + loading state.
    engine::Engine::VoiceView views[engine::VoicePool::numVoices];
    const auto n = engine.getVoiceViews (views, engine::VoicePool::numVoices);

    std::uint32_t sig = engine.isLoading() ? 1u : 0u;
    for (int i = 0; i < n; ++i)
        sig = sig * 131u + static_cast<std::uint32_t> (views[i].chopIndex + 2)
              + static_cast<std::uint32_t> (views[i].normalisedPos * 4096.0f);

    const bool animating = n > 0 || engine.isLoading();
    if (animating || wasAnimating || sig != lastVoiceSignature)
        repaint();

    wasAnimating = animating;
    lastVoiceSignature = sig;
}

// ---- coordinates ------------------------------------------------------------------

float WaveformDisplay::timeToX (double sec) const
{
    if (viewLen <= 0.0)
        return 0.0f;
    return static_cast<float> ((sec - viewStart) / viewLen * getWidth());
}

double WaveformDisplay::xToTime (float x) const
{
    if (getWidth() <= 0)
        return 0.0;
    return viewStart + static_cast<double> (x) / getWidth() * viewLen;
}

int WaveformDisplay::markerIdAt (float x) const
{
    const auto& markers = engine.getMarkers();
    int best = -1;
    float bestDist = markerHitZone + 0.001f;
    for (size_t i = 1; i < markers.size(); ++i) // marker 0 (0 s) is fixed
    {
        const auto dist = std::abs (timeToX (markers[i].timeSec) - x);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = markers[i].id;
        }
    }
    return best;
}

int WaveformDisplay::chopIndexAt (float x) const
{
    const auto spans = chopSpans();
    const auto t = xToTime (x);
    for (size_t i = 0; i < spans.size(); ++i)
        if (t >= spans[i].startSec && t < spans[i].endSec)
            return static_cast<int> (i);
    return spans.empty() ? -1 : (t >= spans.back().endSec ? static_cast<int> (spans.size()) - 1 : -1);
}

void WaveformDisplay::clampView()
{
    const auto length = sampleLengthSeconds();
    if (length <= 0.0)
        return;
    viewLen = juce::jlimit (minViewSeconds, length, viewLen);
    viewStart = juce::jlimit (0.0, length - viewLen, viewStart);
}

// ---- painting ------------------------------------------------------------------------

void WaveformDisplay::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.setColour (ChoplineLookAndFeel::display);
    g.fillRoundedRectangle (area, cornerRadius);

    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (area, cornerRadius);
        g.reduceClipRegion (clip);

        // Hairline grid.
        g.setColour (ChoplineLookAndFeel::gridLine());
        for (auto gx = gridStep; gx < area.getWidth(); gx += gridStep)
            g.drawVerticalLine (static_cast<int> (gx), 0.0f, area.getHeight());
        for (auto gy = gridStep; gy < area.getHeight(); gy += gridStep)
            g.drawHorizontalLine (static_cast<int> (gy), 0.0f, area.getWidth());

        const auto s = sample();
        if (s != nullptr && s->numFrames() > 0 && viewLen > 0.0)
        {
            drawChops (g, area);
            drawWaveform (g, area);
            drawPlayheads (g, area);
        }

        if (s == nullptr || engine.isLoading() || engine.getLastError().isNotEmpty())
            drawEmptyOrLoading (g, area);
    }

    g.setColour (ChoplineLookAndFeel::hairline());
    g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.0f);
}

void WaveformDisplay::drawWaveform (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto s = sample();
    const auto rate = s->sessionRate;
    const auto total = s->numFrames();
    const auto width = juce::jmax (1, getWidth());
    const auto midY = area.getCentreY();
    const auto halfH = area.getHeight() * 0.42f;

    const auto samplesPerPixel = viewLen * rate / width;
    const auto* raw = s->audio.getReadPointer (0);
    const auto* rawR = s->audio.getNumChannels() > 1 ? s->audio.getReadPointer (1) : raw;

    const engine::SampleData::PeakLevel* level = nullptr;
    if (samplesPerPixel >= 2048.0 && s->peaksCoarse.bucketSize > 0)
        level = &s->peaksCoarse;
    else if (samplesPerPixel >= 48.0 && s->peaksFine.bucketSize > 0)
        level = &s->peaksFine;

    g.setColour (ChoplineLookAndFeel::ink);

    for (int px = 0; px < width; ++px)
    {
        const auto t0 = viewStart + static_cast<double> (px) / width * viewLen;
        const auto t1 = viewStart + static_cast<double> (px + 1) / width * viewLen;
        auto s0 = juce::jlimit<juce::int64> (0, total, static_cast<juce::int64> (t0 * rate));
        auto s1 = juce::jlimit<juce::int64> (0, total, static_cast<juce::int64> (t1 * rate) + 1);
        if (s1 <= s0)
            continue;

        float mn = 1.0e9f, mx = -1.0e9f;
        if (level != nullptr)
        {
            const auto b0 = s0 / level->bucketSize;
            const auto b1 = juce::jmin<juce::int64> (static_cast<juce::int64> (level->mins.size()) - 1,
                                                     (s1 - 1) / level->bucketSize);
            for (auto b = b0; b <= b1; ++b)
            {
                mn = juce::jmin (mn, level->mins[static_cast<size_t> (b)]);
                mx = juce::jmax (mx, level->maxs[static_cast<size_t> (b)]);
            }
        }
        else
        {
            for (auto i = s0; i < s1; ++i)
            {
                const auto v = 0.5f * (raw[i] + rawR[i]);
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
            }
        }
        if (mn > mx)
            continue;

        const auto yTop = midY - juce::jlimit (-1.0f, 1.0f, mx) * halfH;
        const auto yBot = midY - juce::jlimit (-1.0f, 1.0f, mn) * halfH;
        g.drawLine (static_cast<float> (px) + 0.5f, yTop,
                    static_cast<float> (px) + 0.5f, juce::jmax (yBot, yTop + 1.0f), 1.0f);
    }
}

void WaveformDisplay::drawChops (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto spans = chopSpans();
    const auto& markers = engine.getMarkers();

    // Which chops are currently sounding (blue flash per v17 strip_feedback)?
    engine::Engine::VoiceView views[engine::VoicePool::numVoices];
    const auto numViews = engine.getVoiceViews (views, engine::VoicePool::numVoices);
    auto isPlaying = [&] (int chop)
    {
        for (int i = 0; i < numViews; ++i)
            if (views[i].chopIndex == chop)
                return true;
        return false;
    };

    for (size_t i = 0; i < spans.size(); ++i)
    {
        const auto x0 = timeToX (spans[i].startSec);
        const auto x1 = timeToX (spans[i].endSec);
        if (x1 < 0.0f || x0 > area.getWidth())
            continue;

        const auto chopIdx = static_cast<int> (i);
        const auto shade = ChoplineLookAndFeel::chopShade (chopIdx);
        const auto band = juce::Rectangle<float> (x0, 0.0f, x1 - x0, area.getHeight());

        auto alpha = 0.08f;
        if (hoverChop == chopIdx)
            alpha = 0.13f;
        g.setColour (shade.withAlpha (alpha));
        g.fillRect (band);

        if (isPlaying (chopIdx))
        {
            g.setColour (ChoplineLookAndFeel::accent.withAlpha (0.07f));
            g.fillRect (band);
        }

        if (selection.count (chopIdx) > 0) // thin blue selection outline
        {
            g.setColour (ChoplineLookAndFeel::accent);
            g.drawRect (band.reduced (0.75f), 1.5f);
        }

        // Index badge (1-based), top-left of the band.
        if (x1 - x0 > 14.0f)
        {
            const auto chip = juce::Rectangle<float> (x0 + 3.0f, 3.0f, 15.0f, 11.0f);
            g.setColour (shade);
            g.fillRoundedRectangle (chip, 3.0f);
            if (selection.count (chopIdx) > 0)
            {
                g.setColour (ChoplineLookAndFeel::accent);
                g.drawRoundedRectangle (chip.expanded (1.0f), 4.0f, 1.0f);
            }
            g.setColour (shade.getPerceivedBrightness() > 0.6f ? ChoplineLookAndFeel::ink
                                                               : juce::Colours::white);
            g.setFont (ChoplineLookAndFeel::monoFont (8.0f));
            g.drawText (juce::String (chopIdx + 1), chip, juce::Justification::centred, false);
        }
    }

    // Slice markers (skip the fixed 0 s marker).
    for (size_t i = 1; i < markers.size(); ++i)
    {
        const auto x = timeToX (markers[i].timeSec);
        if (x < -1.0f || x > area.getWidth() + 1.0f)
            continue;
        const bool hot = markers[i].id == hoverMarkerId || markers[i].id == dragMarkerId;
        g.setColour (hot ? ChoplineLookAndFeel::accent : ChoplineLookAndFeel::ink.withAlpha (0.8f));
        g.drawLine (x, 0.0f, x, area.getHeight(), hot ? 1.6f : 1.0f);
    }
}

void WaveformDisplay::drawPlayheads (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto spans = chopSpans();
    engine::Engine::VoiceView views[engine::VoicePool::numVoices];
    const auto n = engine.getVoiceViews (views, engine::VoicePool::numVoices);

    g.setColour (ChoplineLookAndFeel::accent);
    for (int i = 0; i < n; ++i)
    {
        const auto chop = views[i].chopIndex;
        if (chop < 0 || chop >= static_cast<int> (spans.size()))
            continue;
        const auto& span = spans[static_cast<size_t> (chop)];
        const auto t = span.startSec + views[i].normalisedPos * (span.endSec - span.startSec);
        const auto x = timeToX (t);
        if (x >= 0.0f && x <= area.getWidth())
            g.drawLine (x, 0.0f, x, area.getHeight(), 1.5f);
    }
}

void WaveformDisplay::drawEmptyOrLoading (juce::Graphics& g, juce::Rectangle<float> area)
{
    if (engine.isLoading())
    {
        // Spinner arc + caption.
        const auto phase = static_cast<float> (juce::Time::getMillisecondCounter() % 1000u)
                           / 1000.0f * juce::MathConstants<float>::twoPi;
        juce::Path arc;
        arc.addCentredArc (area.getCentreX(), area.getCentreY() - 8.0f, 9.0f, 9.0f, phase,
                           0.0f, juce::MathConstants<float>::pi * 1.5f, true);
        g.setColour (ChoplineLookAndFeel::textDim);
        g.strokePath (arc, juce::PathStrokeType (1.6f));
        g.setFont (ChoplineLookAndFeel::labelFont (8.5f));
        g.drawText ("LOADING", area.withTop (area.getCentreY() + 8.0f).withHeight (14.0f),
                    juce::Justification::centredTop, false);
        return;
    }

    if (sample() == nullptr)
    {
        g.setColour (ChoplineLookAndFeel::textTertiary);
        g.setFont (ChoplineLookAndFeel::labelFont (9.0f));
        const auto error = engine.getLastError();
        g.drawText (error.isNotEmpty() ? error.toUpperCase()
                                       : juce::String ("DROP A VOCAL OR CLICK LOAD"),
                    area, juce::Justification::centred, false);
    }
}

// ---- interaction --------------------------------------------------------------------------

void WaveformDisplay::mouseMove (const juce::MouseEvent& e)
{
    const auto newMarker = markerIdAt (e.position.x);
    const auto newChop = newMarker >= 0 ? -1 : chopIndexAt (e.position.x);
    if (newMarker != hoverMarkerId || newChop != hoverChop)
    {
        hoverMarkerId = newMarker;
        hoverChop = newChop;
        repaint();
    }
    setMouseCursor (newMarker >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                   : juce::MouseCursor::NormalCursor);
}

void WaveformDisplay::mouseExit (const juce::MouseEvent&)
{
    hoverMarkerId = -1;
    hoverChop = -1;
    repaint();
}

void WaveformDisplay::selectChop (int chopIndex)
{
    selection.clear();
    if (chopIndex >= 0)
        selection.insert (chopIndex);
    repaint();
}

void WaveformDisplay::mouseDown (const juce::MouseEvent& e)
{
    draggedSinceDown = false;
    draggingToGrid = false;
    dragMarkerId = -1;
    downChop = -1;

    if (sample() == nullptr)
        return;

    if (markerIdAt (e.position.x) < 0)
        downChop = chopIndexAt (e.position.x);

    const auto markerId = markerIdAt (e.position.x);

    // Alt-click (or right-click) deletes a marker in Manual mode.
    if (markerId >= 0 && engine.getSliceMode() == engine::SliceMode::manual
        && (e.mods.isAltDown() || e.mods.isPopupMenu()))
    {
        engine.removeMarker (markerId);
        hoverMarkerId = -1;
        return;
    }

    if (markerId >= 0 && ! e.mods.isPopupMenu())
    {
        dragMarkerId = markerId; // draggable in every mode
        return;
    }
}

void WaveformDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 2)
        draggedSinceDown = true;

    if (dragMarkerId >= 0)
    {
        engine.moveMarker (dragMarkerId, xToTime (e.position.x));
        return;
    }

    // Drag a chop (or the multi-selection containing it) onto the grid
    // (spec §5: chops are drag sources for Strip B).
    if (! draggingToGrid && downChop >= 0 && ! e.mods.isAltDown()
        && e.getDistanceFromDragStart() > 6)
    {
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            auto chops = selection.count (downChop) > 0 ? getSelectedChops()
                                                        : std::vector<int> { downChop };
            juce::StringArray list;
            for (auto c : chops)
                list.add (juce::String (c));
            draggingToGrid = true;
            container->startDragging (juce::String (chopDragPrefix) + list.joinIntoString (","),
                                      this, juce::ScaledImage(), true);
        }
    }
}

void WaveformDisplay::mouseUp (const juce::MouseEvent& e)
{
    const auto wasDraggingMarker = dragMarkerId >= 0;
    dragMarkerId = -1;

    if (wasDraggingMarker || draggedSinceDown || sample() == nullptr || e.mods.isPopupMenu()
        || e.mods.isAltDown())
        return;

    const auto chop = chopIndexAt (e.position.x);
    if (chop < 0)
        return;

    if (e.mods.isShiftDown())
    {
        // Multi-select (toggle), no audition.
        if (selection.count (chop) > 0)
            selection.erase (chop);
        else
            selection.insert (chop);
    }
    else
    {
        selection.clear();
        selection.insert (chop);
        engine.audition (chop, 0.9f); // click-to-audition through the voice path
    }
    repaint();
}

void WaveformDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (sample() == nullptr)
        return;
    if (engine.getSliceMode() == engine::SliceMode::manual && markerIdAt (e.position.x) < 0)
        engine.addMarker (xToTime (e.position.x));
}

void WaveformDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto length = sampleLengthSeconds();
    if (length <= 0.0 || viewLen <= 0.0)
        return;

    const auto scrollAmount = std::abs (wheel.deltaX) > std::abs (wheel.deltaY)
                                  ? wheel.deltaX
                                  : (e.mods.isShiftDown() ? wheel.deltaY : 0.0f);

    if (scrollAmount != 0.0f)
    {
        viewStart -= static_cast<double> (scrollAmount) * viewLen * 0.9;
    }
    else if (wheel.deltaY != 0.0f)
    {
        // Zoom around the cursor: the time under the mouse stays put.
        const auto anchor = xToTime (e.position.x);
        const auto factor = std::exp (-static_cast<double> (wheel.deltaY) * 2.4);
        viewLen = juce::jlimit (minViewSeconds, length, viewLen * factor);
        viewStart = anchor - static_cast<double> (e.position.x) / juce::jmax (1, getWidth()) * viewLen;
    }

    clampView();
    repaint();
}

} // namespace chopline::ui
