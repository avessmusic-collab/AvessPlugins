// CHOPLINE — ChopEditorOverlay implementation. See header.

#include "ChopEditorOverlay.h"

#include <cmath>

namespace chopline::ui
{

using LnF = ChoplineLookAndFeel;

namespace
{
    const juce::Colour flagBlue { 0xff5b87f5 };   // v17 editor handles
    const juce::Colour flagHover { 0xffa9bffa };

    juce::String secondsText (double sec) { return juce::String (sec, 3) + " s"; }
} // namespace

// ================================ overlay ==============================================

ChopEditorOverlay::ChopEditorOverlay (engine::Engine& engineToUse)
    : eng (engineToUse)
{
    setWantsKeyboardFocus (true);

    addAndMakeVisible (display);

    modeBox.setName ("Mode");
    modeBox.addItem ("One-shot", 1);
    modeBox.addItem ("Gate", 2);
    modeBox.addItem ("Loop", 3);
    modeBox.addItem ("Fit step", 4);
    modeBox.onChange = [this]
    {
        if (refreshing || modeBox.getSelectedId() == 0)
            return;
        auto s = settings();
        s.mode = static_cast<engine::PlaybackMode> (modeBox.getSelectedId() - 1);
        setSettings (s);
    };
    addAndMakeVisible (modeBox);

    reverseToggle.onClick = [this]
    {
        if (refreshing)
            return;
        auto s = settings();
        s.reverse = reverseToggle.getToggleState();
        setSettings (s);
    };
    addAndMakeVisible (reverseToggle);

    // Mirrors the slicing flyout's zero-cross snap (v17 mirror_of slice_zero_cross).
    snapToggle.onClick = [this]
    {
        if (! refreshing)
            eng.setZeroCrossSnap (snapToggle.getToggleState());
    };
    addAndMakeVisible (snapToggle);

    warpSeg.options = { "Varispeed", "Stretch", "Granular", "Freeze" };
    warpSeg.getValue = [this] { return static_cast<int> (settings().warp); };
    warpSeg.setValue = [this] (int v)
    {
        auto s = settings();
        s.warp = static_cast<engine::WarpMode> (v);
        setSettings (s);
    };
    warpSeg.isOptionEnabled = [] (int i) { return i < 2; }; // Granular/Freeze = Phase 5
    warpSeg.disabledTooltip = "Granular and Freeze arrive in Phase 5";
    addAndMakeVisible (warpSeg);

    closeButton.onClick = [this]
    {
        if (onClose != nullptr)
            onClose();
    };
    addAndMakeVisible (closeButton);

    auto setupKnob = [this] (CompactKnob& k, double lo, double hi, double step,
                             std::function<void (engine::ChopSettings&, double)> apply)
    {
        k.getSlider().setRange (lo, hi, step);
        k.onValueChange = [this, fn = std::move (apply)] (double v)
        {
            if (refreshing)
                return;
            auto s = settings();
            fn (s, v);
            setSettings (s);
        };
        addAndMakeVisible (k);
    };

    setupKnob (pitchKnob, -24.0, 24.0, 1.0,
               [] (engine::ChopSettings& s, double v) { s.pitchSemitones = static_cast<int> (v); });
    pitchKnob.formatValue = [] (double v)
    {
        const auto i = static_cast<int> (v);
        return (i > 0 ? "+" + juce::String (i) : juce::String (i)) + " st";
    };
    pitchKnob.getSlider().setDoubleClickReturnValue (true, 0.0);

    setupKnob (releaseKnob, 5.0, 500.0, 1.0,
               [] (engine::ChopSettings& s, double v) { s.releaseMs = static_cast<float> (v); });
    releaseKnob.valueSuffix = " ms";
    releaseKnob.getSlider().setDoubleClickReturnValue (true, 80.0);

    // DELAY / VERB sends are STORED now and become audible with the Phase 4
    // master delay/reverb (documented in CLAUDE.md).
    setupKnob (delayKnob, 0.0, 100.0, 1.0,
               [] (engine::ChopSettings& s, double v) { s.delaySend = static_cast<float> (v); });
    delayKnob.valueSuffix = "%";
    delayKnob.getSlider().setDoubleClickReturnValue (true, 0.0);

    setupKnob (verbKnob, 0.0, 100.0, 1.0,
               [] (engine::ChopSettings& s, double v) { s.verbSend = static_cast<float> (v); });
    verbKnob.valueSuffix = "%";
    verbKnob.getSlider().setDoubleClickReturnValue (true, 0.0);

    setupKnob (unisonKnob, 1.0, 4.0, 1.0,
               [] (engine::ChopSettings& s, double v) { s.unison = static_cast<int> (v); });
    unisonKnob.formatValue = [] (double v) { return juce::String (static_cast<int> (v)) + "x"; };
    unisonKnob.getSlider().setDoubleClickReturnValue (true, 1.0);

    setupKnob (xfadeKnob, 0.0, 200.0, 1.0,
               [] (engine::ChopSettings& s, double v) { s.loopXfadeMs = static_cast<float> (v); });
    xfadeKnob.valueSuffix = " ms";
    xfadeKnob.getSlider().setDoubleClickReturnValue (true, 10.0);

    eng.addChangeListener (this);
    refresh();
}

ChopEditorOverlay::~ChopEditorOverlay()
{
    eng.removeChangeListener (this);
}

void ChopEditorOverlay::CloseButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (down ? juce::Colour (0xffededf0)
                      : highlighted ? juce::Colour (0xfff2f2f4) : LnF::cell);
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b, 7.0f, 1.0f);
    g.setColour (LnF::label);
    const auto c = b.getCentre();
    g.drawLine (c.x - 3.5f, c.y - 3.5f, c.x + 3.5f, c.y + 3.5f, 1.4f);
    g.drawLine (c.x - 3.5f, c.y + 3.5f, c.x + 3.5f, c.y - 3.5f, 1.4f);
}

void ChopEditorOverlay::setChop (int chopIndex)
{
    chop = juce::jmax (0, chopIndex);
    display.refitView();
    refresh();
}

void ChopEditorOverlay::animationTick()
{
    if (isVisible())
        display.animationTick();
}

void ChopEditorOverlay::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (! isVisible())
        return;
    const auto numChops = eng.getNumChops();
    if (numChops <= 0)
    {
        if (onClose != nullptr)
            onClose(); // sample/slices vanished under the overlay
        return;
    }
    if (chop >= numChops)
        chop = numChops - 1;
    refresh();
}

void ChopEditorOverlay::refresh()
{
    refreshing = true;
    const auto s = settings();
    modeBox.setSelectedId (static_cast<int> (s.mode) + 1, juce::dontSendNotification);
    reverseToggle.setToggleState (s.reverse, juce::dontSendNotification);
    snapToggle.setToggleState (eng.getZeroCrossSnap(), juce::dontSendNotification);
    pitchKnob.setValueQuiet (s.pitchSemitones);
    releaseKnob.setValueQuiet (s.releaseMs);
    delayKnob.setValueQuiet (s.delaySend);
    verbKnob.setValueQuiet (s.verbSend);
    unisonKnob.setValueQuiet (s.unison);
    xfadeKnob.setValueQuiet (s.loopXfadeMs);
    const bool loop = s.mode == engine::PlaybackMode::loop;
    xfadeKnob.setEnabled (loop); // v17: dimmed unless mode = Loop
    warpSeg.repaint();
    refreshing = false;
    repaint();
    display.repaint();
}

ChopEditorOverlay::ChopView ChopEditorOverlay::computeView() const
{
    ChopView v;
    const auto sample = eng.currentSample();
    const auto& markers = eng.getMarkers();
    const auto i = static_cast<size_t> (chop);
    if (sample == nullptr || markers.empty() || i >= markers.size())
        return v;

    const auto lengthSec = sample->lengthSeconds();
    v.minLen = static_cast<double> (eng.getMinSliceLengthMs()) * 0.001;
    v.baseStart = markers[i].timeSec;
    v.baseEnd = i + 1 < markers.size() ? markers[i + 1].timeSec : lengthSec;
    v.prevBound = i > 0 ? markers[i - 1].timeSec : 0.0;
    v.nextBound = i + 2 < markers.size() ? markers[i + 2].timeSec : lengthSec;

    const auto s = eng.getChopSettings (chop);
    auto effEnd = v.baseEnd + s.endOffsetSec;
    effEnd = juce::jlimit (juce::jmin (v.baseStart + v.minLen, v.nextBound), v.nextBound, effEnd);
    auto startHi = effEnd - v.minLen;
    if (startHi < v.prevBound)
        startHi = v.prevBound;
    v.effStart = juce::jlimit (v.prevBound, startHi, v.baseStart + s.startOffsetSec);
    v.effEnd = effEnd;
    v.valid = v.effEnd > v.effStart;
    return v;
}

void ChopEditorOverlay::resized()
{
    // v17 canvas-absolute geometry (the overlay fills the 1100x720 canvas).
    display.setBounds (166, 164, 768, 260);
    closeButton.setBounds (906, 134, 28, 20);
    modeBox.setBounds (166, 434, 160, 28);
    reverseToggle.setBounds (336, 434, 110, 28);
    snapToggle.setBounds (456, 434, 140, 28);
    warpSeg.setBounds (606, 434, 328, 28);
    pitchKnob.setBounds (166, 512, 52, 60);
    releaseKnob.setBounds (309, 512, 52, 60);
    delayKnob.setBounds (452, 512, 52, 60);
    verbKnob.setBounds (595, 512, 52, 60);
    unisonKnob.setBounds (738, 512, 52, 60);
    xfadeKnob.setBounds (882, 512, 52, 60);
}

void ChopEditorOverlay::paint (juce::Graphics& g)
{
    // Scrim + panel.
    g.fillAll (juce::Colours::black.withAlpha (0.28f));
    const auto panel = juce::Rectangle<float> (panelX, panelY, panelW, panelH);
    g.setColour (juce::Colours::black.withAlpha (0.10f));
    g.fillRoundedRectangle (panel.translated (0.0f, 2.0f), 16.0f);
    g.setColour (juce::Colour (0xfffafafa));
    g.fillRoundedRectangle (panel, 16.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (panel.reduced (0.5f), 16.0f, 1.0f);

    // Chip: swatch + CHOP nn (v17 overlay_chip).
    {
        const auto shade = LnF::chopShade (chop);
        const auto swatch = juce::Rectangle<float> (166.0f, 140.0f, 10.0f, 10.0f);
        g.setColour (shade);
        g.fillRoundedRectangle (swatch, 2.5f);
        g.setColour (LnF::accent);
        g.drawRoundedRectangle (swatch.expanded (1.5f), 3.5f, 1.0f);
        g.setColour (LnF::label);
        g.setFont (LnF::labelFont (8.5f));
        g.drawText ("CHOP " + juce::String (chop + 1),
                    juce::Rectangle<int> (184, 136, 130, 18),
                    juce::Justification::centredLeft, false);
    }

    // Readouts (v17 ed_*): label (faint) over value (ink; blue while dragged).
    const auto view = computeView();
    const auto s = settings();
    const auto dragKind = display.currentDrag();
    struct Readout
    {
        const char* caption;
        juce::String value;
        int x, w;
        bool hot;
    };
    using H = EditorDisplay::Handle;
    const Readout readouts[] = {
        { "START", view.valid ? secondsText (view.effStart) : juce::String ("--"),
          166, 120, dragKind == H::startFlag },
        { "END", view.valid ? secondsText (view.effEnd) : juce::String ("--"),
          296, 104, dragKind == H::endFlag },
        { "LENGTH", view.valid ? secondsText (view.effEnd - view.effStart) : juce::String ("--"),
          410, 120, dragKind == H::startFlag || dragKind == H::endFlag },
        { "FADE IN", juce::String (s.fadeInMs, 1) + " ms", 540, 130, dragKind == H::fadeIn },
        { "FADE OUT", juce::String (s.fadeOutMs, 1) + " ms", 680, 138, dragKind == H::fadeOut },
        { "LOOP",
          s.mode == engine::PlaybackMode::loop
              ? juce::String (static_cast<int> (std::lround (s.loopStart * 100.0))) + "-"
                    + juce::String (static_cast<int> (std::lround (s.loopEnd * 100.0))) + "%"
              : juce::String ("OFF"),
          828, 106, dragKind == H::loopStart || dragKind == H::loopEnd || dragKind == H::xfade },
    };
    for (const auto& r : readouts)
    {
        g.setColour (LnF::textTertiary);
        g.setFont (LnF::labelFont (8.0f));
        g.drawText (r.caption, juce::Rectangle<int> (r.x, 472, r.w, 10),
                    juce::Justification::centredLeft, false);
        g.setColour (r.hot ? LnF::accent : LnF::ink);
        g.setFont (LnF::monoFont (10.5f));
        g.drawText (r.value, juce::Rectangle<int> (r.x, 484, r.w, 14),
                    juce::Justification::centredLeft, false);
    }
}

void ChopEditorOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! juce::Rectangle<int> (panelX, panelY, panelW, panelH).contains (e.getPosition())
        && onClose != nullptr)
        onClose(); // scrim click dismisses (v17)
}

bool ChopEditorOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        if (onClose != nullptr)
            onClose();
        return true;
    }
    return false;
}

void ChopEditorOverlay::visibilityChanged()
{
    if (isVisible())
    {
        display.refitView();
        refresh();
        grabKeyboardFocus();
    }
}

// ============================ EditorDisplay =============================================

float ChopEditorOverlay::EditorDisplay::timeToX (double sec) const
{
    if (viewLen <= 0.0)
        return 0.0f;
    return static_cast<float> ((sec - viewStart) / viewLen * getWidth());
}

double ChopEditorOverlay::EditorDisplay::xToTime (float x) const
{
    if (getWidth() <= 0)
        return 0.0;
    return viewStart + static_cast<double> (x) / getWidth() * viewLen;
}

void ChopEditorOverlay::EditorDisplay::refitView()
{
    const auto view = owner.computeView();
    if (! view.valid)
    {
        viewStart = 0.0;
        viewLen = 0.0;
        return;
    }
    // v17: chop ±30%, clamped to the neighbour slices.
    const auto len = view.effEnd - view.effStart;
    viewStart = juce::jmax (view.prevBound, view.effStart - 0.3 * len);
    const auto viewEnd = juce::jmin (view.nextBound, view.effEnd + 0.3 * len);
    viewLen = juce::jmax (1.0e-4, viewEnd - viewStart);
    repaint();
}

void ChopEditorOverlay::EditorDisplay::animationTick()
{
    engine::Engine::VoiceView views[engine::VoicePool::numVoices];
    const auto n = owner.eng.getVoiceViews (views, engine::VoicePool::numVoices);
    bool animating = false;
    for (int i = 0; i < n; ++i)
        animating = animating || views[i].chopIndex == owner.chop;
    if (animating || wasAnimating)
        repaint();
    wasAnimating = animating;
}

void ChopEditorOverlay::EditorDisplay::drawWave (juce::Graphics& g, juce::Rectangle<float> area,
                                                 const ChopView& view)
{
    const auto sample = owner.eng.currentSample();
    if (sample == nullptr || sample->numFrames() <= 0 || viewLen <= 0.0)
        return;

    const auto rate = sample->sessionRate;
    const auto total = sample->numFrames();
    const auto width = juce::jmax (1, getWidth());
    const auto midY = area.getCentreY();
    const auto halfH = area.getHeight() * 0.40f;

    const auto samplesPerPixel = viewLen * rate / width;
    const auto* raw = sample->audio.getReadPointer (0);
    const auto* rawR = sample->audio.getNumChannels() > 1 ? sample->audio.getReadPointer (1) : raw;

    const engine::SampleData::PeakLevel* level = nullptr;
    if (samplesPerPixel >= 2048.0 && sample->peaksCoarse.bucketSize > 0)
        level = &sample->peaksCoarse;
    else if (samplesPerPixel >= 48.0 && sample->peaksFine.bucketSize > 0)
        level = &sample->peaksFine;

    for (int px = 0; px < width; ++px)
    {
        const auto t0 = viewStart + static_cast<double> (px) / width * viewLen;
        const auto t1 = viewStart + static_cast<double> (px + 1) / width * viewLen;
        const auto s0 = juce::jlimit<juce::int64> (0, total, static_cast<juce::int64> (t0 * rate));
        const auto s1 = juce::jlimit<juce::int64> (0, total, static_cast<juce::int64> (t1 * rate) + 1);
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

        const auto mid = 0.5 * (t0 + t1);
        const bool inside = mid >= view.effStart && mid < view.effEnd;
        g.setColour (inside ? LnF::ink : LnF::textTertiary.withAlpha (0.8f));
        const auto yTop = midY - juce::jlimit (-1.0f, 1.0f, mx) * halfH;
        const auto yBot = midY - juce::jlimit (-1.0f, 1.0f, mn) * halfH;
        g.drawLine (static_cast<float> (px) + 0.5f, yTop,
                    static_cast<float> (px) + 0.5f, juce::jmax (yBot, yTop + 1.0f), 1.0f);
    }
}

void ChopEditorOverlay::EditorDisplay::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const auto h = area.getHeight();

    g.setColour (LnF::display);
    g.fillRoundedRectangle (area, 10.0f);

    juce::Graphics::ScopedSaveState save (g);
    juce::Path clip;
    clip.addRoundedRectangle (area, 10.0f);
    g.reduceClipRegion (clip);

    // Hairline grid (v17: 22 px).
    g.setColour (LnF::gridLine());
    for (auto gx = 22.0f; gx < area.getWidth(); gx += 22.0f)
        g.drawVerticalLine (static_cast<int> (gx), 0.0f, h);
    for (auto gy = 22.0f; gy < h; gy += 22.0f)
        g.drawHorizontalLine (static_cast<int> (gy), 0.0f, area.getWidth());

    const auto view = owner.computeView();
    const auto sample = owner.eng.currentSample();
    if (! view.valid || sample == nullptr || viewLen <= 0.0)
    {
        g.setColour (LnF::textTertiary);
        g.setFont (LnF::labelFont (8.5f));
        g.drawText ("NO CHOP", getLocalBounds(), juce::Justification::centred, false);
        g.setColour (LnF::hairline());
        g.drawRoundedRectangle (area.reduced (0.5f), 10.0f, 1.0f);
        return;
    }

    const auto s = owner.settings();
    const auto startX = timeToX (view.effStart);
    const auto endX = timeToX (view.effEnd);

    // Faint blue tint on the trimmed chop (v17: "trimmed chop bright on a
    // faint blue tint").
    g.setColour (LnF::accent.withAlpha (0.06f));
    g.fillRect (juce::Rectangle<float> (startX, 0.0f, endX - startX, h));

    drawWave (g, area, view);

    // Dashed slice markers (the untrimmed slice bounds).
    {
        const float dashes[] = { 3.0f, 3.0f };
        g.setColour (LnF::ink.withAlpha (0.4f));
        for (const auto t : { view.baseStart, view.baseEnd })
        {
            const auto x = timeToX (t);
            if (x >= 0.0f && x <= area.getWidth())
                g.drawDashedLine ({ { x, 0.0f }, { x, h } }, dashes, 2, 1.0f);
        }
    }

    // Fade wedges + top-corner grips.
    const auto secPerMs = 0.001;
    const auto fadeInX = timeToX (view.effStart + s.fadeInMs * secPerMs);
    const auto fadeOutX = timeToX (view.effEnd - s.fadeOutMs * secPerMs);
    {
        g.setColour (flagBlue.withAlpha (0.5f));
        g.drawLine (startX, h, fadeInX, 10.0f, 1.2f); // fade-in envelope wedge
        g.drawLine (endX, h, fadeOutX, 10.0f, 1.2f);  // fade-out wedge

        auto grip = [&] (float x, bool hot)
        {
            juce::Path p; // down-pointing triangle at the top
            p.addTriangle (x - 5.0f, 2.0f, x + 5.0f, 2.0f, x, 10.0f);
            g.setColour (hot ? flagHover : flagBlue);
            g.fillPath (p);
        };
        grip (fadeInX, hover == Handle::fadeIn || drag == Handle::fadeIn);
        grip (fadeOutX, hover == Handle::fadeOut || drag == Handle::fadeOut);
    }

    // Loop brace (Loop mode only).
    if (s.mode == engine::PlaybackMode::loop)
    {
        const auto len = view.effEnd - view.effStart;
        const auto lsX = timeToX (view.effStart + s.loopStart * len);
        const auto leX = timeToX (view.effStart + s.loopEnd * len);

        g.setColour (LnF::accent.withAlpha (0.08f));
        g.fillRect (juce::Rectangle<float> (lsX, 0.0f, leX - lsX, h));
        g.setColour (flagBlue);
        g.drawLine (lsX, 14.0f, leX, 14.0f, 1.2f); // brace line

        auto bar = [&] (float x, bool hot)
        {
            g.setColour (hot ? flagHover : flagBlue);
            g.drawLine (x, 0.0f, x, h, 1.4f);
            g.fillRoundedRectangle (x - 4.0f, h - 14.0f, 8.0f, 10.0f, 2.0f); // bottom grip
        };
        bar (lsX, hover == Handle::loopStart || drag == Handle::loopStart);
        bar (leX, hover == Handle::loopEnd || drag == Handle::loopEnd);

        // Crossfade diamond + glyph on the brace.
        const auto hot = hover == Handle::xfade || drag == Handle::xfade;
        juce::Path diamond;
        const auto dx = lsX + 14.0f;
        diamond.addQuadrilateral (dx, 8.0f, dx + 6.0f, 14.0f, dx, 20.0f, dx - 6.0f, 14.0f);
        g.setColour (hot ? flagHover : flagBlue);
        g.fillPath (diamond);
        g.setColour (juce::Colours::white);
        g.setFont (LnF::monoFont (7.0f));
        g.drawText ("x", juce::Rectangle<int> (static_cast<int> (dx) - 4, 10, 8, 8),
                    juce::Justification::centred, false);
    }

    // START / END flags (S right of the start line, E left of the end line).
    auto flag = [&] (float x, bool isStart, bool hot)
    {
        g.setColour (hot ? flagHover : flagBlue);
        g.drawLine (x, 0.0f, x, h, 1.5f);
        const auto tab = isStart
            ? juce::Rectangle<float> (x, 18.0f, 13.0f, 13.0f)
            : juce::Rectangle<float> (x - 13.0f, 18.0f, 13.0f, 13.0f);
        g.fillRect (tab);
        g.setColour (juce::Colours::white);
        g.setFont (LnF::labelFont (7.5f));
        g.drawText (isStart ? "S" : "E", tab.toNearestInt(), juce::Justification::centred, false);
    };
    flag (startX, true, hover == Handle::startFlag || drag == Handle::startFlag);
    flag (endX, false, hover == Handle::endFlag || drag == Handle::endFlag);

    // Direction chevron (points left when Reverse — v17 handles.direction).
    {
        const auto cx = area.getWidth() - 18.0f;
        const auto cy = 12.0f;
        const auto dir = s.reverse ? -1.0f : 1.0f;
        g.setColour (LnF::textDim);
        for (int i = 0; i < 2; ++i)
        {
            const auto off = static_cast<float> (i) * 6.0f * dir;
            juce::Path chevron;
            chevron.startNewSubPath (cx + off - 3.0f * dir, cy - 4.0f);
            chevron.lineTo (cx + off + 3.0f * dir, cy);
            chevron.lineTo (cx + off - 3.0f * dir, cy + 4.0f);
            g.strokePath (chevron, juce::PathStrokeType (1.4f));
        }
    }

    // Seconds ruler (SF Mono 9 px, bottom).
    {
        g.setFont (LnF::monoFont (9.0f));
        const auto niceSteps = { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0 };
        auto step = 1.0;
        for (const auto candidate : niceSteps)
            if (viewLen / candidate <= 9.0)
            {
                step = candidate;
                break;
            }
        for (auto t = std::ceil (viewStart / step) * step; t < viewStart + viewLen; t += step)
        {
            const auto x = timeToX (t);
            g.setColour (LnF::gridLine());
            g.drawLine (x, h - 10.0f, x, h, 1.0f);
            g.setColour (LnF::textTertiary);
            g.drawText (juce::String (t, step < 0.1 ? 2 : 1),
                        juce::Rectangle<int> (static_cast<int> (x) + 3, static_cast<int> (h) - 13, 44, 11),
                        juce::Justification::centredLeft, false);
        }
    }

    // Audition playheads of this chop (voice position over the EFFECTIVE chop).
    {
        engine::Engine::VoiceView views[engine::VoicePool::numVoices];
        const auto n = owner.eng.getVoiceViews (views, engine::VoicePool::numVoices);
        g.setColour (LnF::accent);
        for (int i = 0; i < n; ++i)
        {
            if (views[i].chopIndex != owner.chop)
                continue;
            const auto t = view.effStart
                         + static_cast<double> (views[i].normalisedPos) * (view.effEnd - view.effStart);
            const auto x = timeToX (t);
            if (x >= 0.0f && x <= area.getWidth())
                g.drawLine (x, 0.0f, x, h, 1.5f);
        }
    }

    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (area.reduced (0.5f), 10.0f, 1.0f);
}

ChopEditorOverlay::EditorDisplay::Handle
ChopEditorOverlay::EditorDisplay::handleAt (juce::Point<float> p) const
{
    const auto view = owner.computeView();
    if (! view.valid || viewLen <= 0.0)
        return Handle::none;
    const auto s = owner.settings();
    const auto h = static_cast<float> (getHeight());

    const auto startX = timeToX (view.effStart);
    const auto endX = timeToX (view.effEnd);
    const auto fadeInX = timeToX (view.effStart + s.fadeInMs * 0.001);
    const auto fadeOutX = timeToX (view.effEnd - s.fadeOutMs * 0.001);

    if (s.mode == engine::PlaybackMode::loop)
    {
        const auto len = view.effEnd - view.effStart;
        const auto lsX = timeToX (view.effStart + s.loopStart * len);
        const auto leX = timeToX (view.effStart + s.loopEnd * len);
        if (p.getDistanceFrom ({ lsX + 14.0f, 14.0f }) <= 8.0f)
            return Handle::xfade;
        if (std::abs (p.x - lsX) <= 6.0f && p.y > h - 20.0f)
            return Handle::loopStart;
        if (std::abs (p.x - leX) <= 6.0f && p.y > h - 20.0f)
            return Handle::loopEnd;
    }

    if (p.y <= 20.0f) // fade grips live at the top corners
    {
        if (std::abs (p.x - fadeInX) <= 8.0f)
            return Handle::fadeIn;
        if (std::abs (p.x - fadeOutX) <= 8.0f)
            return Handle::fadeOut;
    }

    if (std::abs (p.x - startX) <= 7.0f) // v17: ±7 px hit zone
        return Handle::startFlag;
    if (std::abs (p.x - endX) <= 7.0f)
        return Handle::endFlag;

    return Handle::none;
}

void ChopEditorOverlay::EditorDisplay::mouseMove (const juce::MouseEvent& e)
{
    const auto newHover = handleAt (e.position);
    if (newHover != hover)
    {
        hover = newHover;
        repaint();
    }
    setMouseCursor (newHover == Handle::none ? juce::MouseCursor::NormalCursor
                                             : juce::MouseCursor::LeftRightResizeCursor);
}

void ChopEditorOverlay::EditorDisplay::mouseExit (const juce::MouseEvent&)
{
    hover = Handle::none;
    repaint();
}

void ChopEditorOverlay::EditorDisplay::mouseDown (const juce::MouseEvent& e)
{
    drag = handleAt (e.position);
    if (drag == Handle::xfade)
    {
        dragRefMs = owner.settings().loopXfadeMs;
        dragRefX = e.position.x;
    }
    if (drag != Handle::none)
    {
        owner.repaint(); // readout turns blue
        return;
    }

    // Click in the waveform = audition from that point (v17 handles.audition).
    const auto view = owner.computeView();
    if (! view.valid)
        return;
    const auto t = juce::jlimit (view.effStart, view.effEnd, xToTime (e.position.x));
    const auto len = juce::jmax (1.0e-9, view.effEnd - view.effStart);
    const auto s = owner.settings();
    const auto norm = s.reverse ? (view.effEnd - t) / len : (t - view.effStart) / len;
    owner.eng.audition (owner.chop, 0.9f, norm);
}

void ChopEditorOverlay::EditorDisplay::applyDrag (const juce::MouseEvent& e)
{
    const auto view = owner.computeView();
    if (! view.valid)
        return;
    auto s = owner.settings();
    const auto sample = owner.eng.currentSample();

    auto snapped = [&] (double t)
    {
        if (owner.eng.getZeroCrossSnap() && sample != nullptr)
            return engine::SliceModel::snapToZeroCross (*sample, t);
        return t;
    };

    const auto t = xToTime (e.position.x);
    const auto len = juce::jmax (1.0e-9, view.effEnd - view.effStart);

    switch (drag)
    {
        case Handle::startFlag:
        {
            auto nt = juce::jlimit (view.prevBound,
                                    juce::jmax (view.prevBound, view.effEnd - view.minLen),
                                    snapped (t));
            s.startOffsetSec = nt - view.baseStart;
            break;
        }
        case Handle::endFlag:
        {
            auto nt = juce::jlimit (juce::jmin (view.effStart + view.minLen, view.nextBound),
                                    view.nextBound, snapped (t));
            s.endOffsetSec = nt - view.baseEnd;
            break;
        }
        case Handle::fadeIn:
            s.fadeInMs = static_cast<float> (juce::jlimit (0.0, 20.0,
                             (t - view.effStart) * 1000.0));
            break;
        case Handle::fadeOut:
            s.fadeOutMs = static_cast<float> (juce::jlimit (0.0, 20.0,
                              (view.effEnd - t) * 1000.0));
            break;
        case Handle::loopStart:
            s.loopStart = juce::jlimit (0.0, s.loopEnd - 0.05, (t - view.effStart) / len);
            break;
        case Handle::loopEnd:
            s.loopEnd = juce::jlimit (s.loopStart + 0.05, 1.0, (t - view.effStart) / len);
            break;
        case Handle::xfade:
            // v17 mockup mapping: ~60 px of travel = 200 ms.
            s.loopXfadeMs = static_cast<float> (juce::jlimit (0.0, 200.0,
                                dragRefMs + (e.position.x - dragRefX) / 60.0 * 200.0));
            break;
        case Handle::none:
            return;
    }
    owner.setSettings (s);
}

void ChopEditorOverlay::EditorDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (drag != Handle::none)
        applyDrag (e);
}

void ChopEditorOverlay::EditorDisplay::mouseUp (const juce::MouseEvent&)
{
    if (drag != Handle::none)
    {
        drag = Handle::none;
        owner.repaint();
        repaint();
    }
}

void ChopEditorOverlay::EditorDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click a flag resets its trim (v17 handles.start_end).
    const auto handle = handleAt (e.position);
    if (handle != Handle::startFlag && handle != Handle::endFlag)
        return;
    auto s = owner.settings();
    if (handle == Handle::startFlag)
        s.startOffsetSec = 0.0;
    else
        s.endOffsetSec = 0.0;
    owner.setSettings (s);
    refitView();
}

} // namespace chopline::ui
