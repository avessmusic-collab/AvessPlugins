// CHOPLINE — FxPage implementation. See header.

#include "FxPage.h"

#include <cmath>

namespace chopline::ui
{

using LnF = ChoplineLookAndFeel;

FxPage::FxPage (engine::Engine& engineToUse)
    : eng (engineToUse)
{
    // PLAYBACK card — mirrors of the overlay controls (v17 mirror_of).
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

    prevButton.onClick = [this]
    {
        if (setChop != nullptr && getChop != nullptr)
            setChop (juce::jmax (0, getChop() - 1));
    };
    nextButton.onClick = [this]
    {
        if (setChop != nullptr && getChop != nullptr)
            setChop (juce::jmin (juce::jmax (0, eng.getNumChops() - 1), getChop() + 1));
    };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);

    addAndMakeVisible (envDisplay);

    auto setupKnob = [this] (CompactKnob& k, double lo, double hi, double step, double resetTo,
                             std::function<void (engine::ChopSettings&, double)> apply)
    {
        k.getSlider().setRange (lo, hi, step);
        k.getSlider().setDoubleClickReturnValue (true, resetTo);
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

    setupKnob (attackKnob, 0.0, 500.0, 1.0, 2.0,
               [] (engine::ChopSettings& s, double v) { s.attackMs = static_cast<float> (v); });
    attackKnob.valueSuffix = " ms";
    setupKnob (holdKnob, 0.0, 500.0, 1.0, 0.0,
               [] (engine::ChopSettings& s, double v) { s.holdMs = static_cast<float> (v); });
    holdKnob.valueSuffix = " ms";
    setupKnob (decayKnob, 5.0, 1000.0, 1.0, 160.0,
               [] (engine::ChopSettings& s, double v) { s.decayMs = static_cast<float> (v); });
    decayKnob.valueSuffix = " ms";
    setupKnob (sustainKnob, 0.0, 100.0, 1.0, 100.0,
               [] (engine::ChopSettings& s, double v) { s.sustainPct = static_cast<float> (v); });
    sustainKnob.valueSuffix = "%";
    setupKnob (releaseKnob, 5.0, 500.0, 1.0, 80.0,
               [] (engine::ChopSettings& s, double v) { s.releaseMs = static_cast<float> (v); });
    releaseKnob.valueSuffix = " ms";

    eng.addChangeListener (this);
    refresh();
}

FxPage::~FxPage()
{
    eng.removeChangeListener (this);
}

void FxPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (isVisible()) // hidden page re-syncs when shown (setPage calls refresh)
        refresh();
}

void FxPage::refresh()
{
    refreshing = true;
    const auto s = settings();
    modeBox.setSelectedId (static_cast<int> (s.mode) + 1, juce::dontSendNotification);
    reverseToggle.setToggleState (s.reverse, juce::dontSendNotification);
    attackKnob.setValueQuiet (s.attackMs);
    holdKnob.setValueQuiet (s.holdMs);
    decayKnob.setValueQuiet (s.decayMs);
    sustainKnob.setValueQuiet (s.sustainPct);
    releaseKnob.setValueQuiet (s.releaseMs);
    warpSeg.repaint();
    envDisplay.repaint();
    refreshing = false;
    repaint();
}

void FxPage::resized()
{
    // v17 canvas-absolute geometry (the page fills the 1100x720 canvas).
    modeBox.setBounds (96, 128, 158, 28);
    reverseToggle.setBounds (270, 128, 158, 28);
    warpSeg.setBounds (96, 166, 332, 28);
    prevButton.setBounds (382, 62, 24, 20);
    nextButton.setBounds (418, 62, 24, 20);
    envDisplay.setBounds (96, 253, 332, 176);
    attackKnob.setBounds (96, 441, 50, 54);
    holdKnob.setBounds (167, 441, 50, 54);
    decayKnob.setBounds (237, 441, 50, 54);
    sustainKnob.setBounds (308, 441, 50, 54);
    releaseKnob.setBounds (378, 441, 50, 54);
}

void FxPage::paint (juce::Graphics& g)
{
    auto card = [&g] (juce::Rectangle<float> r, const juce::String& title)
    {
        g.setColour (LnF::cell);
        g.fillRoundedRectangle (r, 12.0f);
        g.setColour (LnF::hairline());
        g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);
        g.setColour (LnF::label);
        g.setFont (LnF::labelFont (8.5f));
        g.drawText (title, juce::Rectangle<int> (static_cast<int> (r.getX()) + 10,
                                                 static_cast<int> (r.getY()) + 8, 200, 12),
                    juce::Justification::centredLeft, false);
    };

    // Column titles (transparent containers — v17 layout_fx).
    g.setColour (LnF::label);
    g.setFont (LnF::labelFont (8.5f));
    g.drawText ("CHOP", juce::Rectangle<int> (82, 64, 80, 14),
                juce::Justification::centredLeft, false);
    g.drawText ("MASTER", juce::Rectangle<int> (458, 64, 80, 14),
                juce::Justification::centredLeft, false);

    // Chip: swatch + CHOP nn (v17 fx_chop_chip at 254,63).
    {
        const auto chop = chopIndex();
        const auto shade = LnF::chopShade (chop);
        const auto swatch = juce::Rectangle<float> (254.0f, 67.0f, 10.0f, 10.0f);
        g.setColour (shade);
        g.fillRoundedRectangle (swatch, 2.5f);
        g.setColour (LnF::accent);
        g.drawRoundedRectangle (swatch.expanded (1.5f), 3.5f, 1.0f);
        g.setColour (LnF::label);
        g.setFont (LnF::labelFont (8.5f));
        g.drawText ("CHOP " + juce::String (chop + 1),
                    juce::Rectangle<int> (272, 63, 98, 18),
                    juce::Justification::centredLeft, false);
    }

    card ({ 82.0f, 92.0f, 360.0f, 114.0f }, "PLAYBACK");
    card ({ 82.0f, 221.0f, 360.0f, 288.0f }, "ENVELOPE");

    // MASTER column: Phase 4 placeholder.
    const auto master = juce::Rectangle<float> (458.0f, 92.0f, 560.0f, 613.0f);
    g.setColour (LnF::paper);
    g.fillRoundedRectangle (master, 12.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (master.reduced (0.5f), 12.0f, 1.0f);
    g.setColour (LnF::textTertiary);
    g.setFont (LnF::labelFont (8.5f));
    g.drawText (juce::String (juce::CharPointer_UTF8 ("MASTER \xe2\x80\x94 PHASE 4")),
                master.toNearestInt(), juce::Justification::centred, false);
}

// ================================ EnvDisplay =============================================

FxPage::EnvDisplay::Nodes FxPage::EnvDisplay::computeNodes (const engine::ChopSettings& s) const
{
    Nodes n;
    n.area = getLocalBounds().toFloat().reduced (10.0f, 14.0f);
    const auto w = n.area.getWidth();
    const auto top = n.area.getY();
    const auto bottom = n.area.getBottom();

    // Segment widths: each stage gets a max share of the width, scaled by its
    // value relative to its range (A 22%, H 16%, D 22%, S fixed 16%, R 24%).
    const auto xA = n.area.getX() + w * 0.22f * (s.attackMs / 500.0f);
    const auto xH = xA + w * 0.16f * (s.holdMs / 500.0f);
    const auto xD = xH + w * 0.22f * (s.decayMs / 1000.0f);
    const auto xS = xD + w * 0.16f;
    const auto xR = xS + w * 0.24f * (s.releaseMs / 500.0f);
    const auto ySus = bottom - (bottom - top) * (s.sustainPct / 100.0f);

    n.a = { xA, top };
    n.h = { xH, top };
    n.d = { xD, ySus };
    n.s = { xS, ySus };
    n.r = { xR, bottom };
    return n;
}

void FxPage::EnvDisplay::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (LnF::display);
    g.fillRoundedRectangle (area, 10.0f);

    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (area, 10.0f);
        g.reduceClipRegion (clip);

        g.setColour (LnF::gridLine());
        for (auto gx = 22.0f; gx < area.getWidth(); gx += 22.0f)
            g.drawVerticalLine (static_cast<int> (gx), 0.0f, area.getHeight());
        for (auto gy = 22.0f; gy < area.getHeight(); gy += 22.0f)
            g.drawHorizontalLine (static_cast<int> (gy), 0.0f, area.getWidth());

        const auto s = owner.settings();
        const auto n = computeNodes (s);

        // Envelope polyline (dark stroke, v17 display tokens).
        juce::Path env;
        env.startNewSubPath (n.area.getX(), n.area.getBottom());
        env.lineTo (n.a);
        env.lineTo (n.h);
        env.lineTo (n.d);
        env.lineTo (n.s);
        env.lineTo (n.r);
        g.setColour (LnF::ink);
        g.strokePath (env, juce::PathStrokeType (1.8f));

        // Soft fill under the curve.
        auto fill = env;
        fill.lineTo (n.r.x, n.area.getBottom());
        fill.closeSubPath();
        g.setColour (LnF::ink.withAlpha (0.05f));
        g.fillPath (fill);

        // Draggable nodes: ring circles (white fill, dark stroke; blue when hot).
        struct NodeView { juce::Point<float> p; Handle handle; };
        const NodeView nodes[] = {
            { n.a, Handle::attack },
            { n.h, Handle::hold },
            { n.d, Handle::decaySustain },
            { n.r, Handle::release },
        };
        for (const auto& node : nodes)
        {
            const bool hot = node.handle == hover || node.handle == drag;
            g.setColour (juce::Colours::white);
            g.fillEllipse (node.p.x - 4.5f, node.p.y - 4.5f, 9.0f, 9.0f);
            g.setColour (hot ? LnF::accent : LnF::ink);
            g.drawEllipse (node.p.x - 4.5f, node.p.y - 4.5f, 9.0f, 9.0f, hot ? 1.8f : 1.4f);
        }

        // Value of the hovered / dragged handle only (v17 env_display).
        const auto active = drag != Handle::none ? drag : hover;
        if (active != Handle::none)
        {
            juce::String text;
            juce::Point<float> at;
            switch (active)
            {
                case Handle::attack:       text = juce::String (s.attackMs, 0) + " ms";  at = n.a; break;
                case Handle::hold:         text = juce::String (s.holdMs, 0) + " ms";    at = n.h; break;
                case Handle::decaySustain: text = juce::String (s.decayMs, 0) + " ms / "
                                                  + juce::String (s.sustainPct, 0) + "%"; at = n.d; break;
                case Handle::release:      text = juce::String (s.releaseMs, 0) + " ms"; at = n.r; break;
                case Handle::none:         break;
            }
            g.setFont (LnF::monoFont (9.0f));
            const auto tw = static_cast<float> (
                juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 10);
            auto pill = juce::Rectangle<float> (tw, 14.0f)
                            .withCentre ({ juce::jlimit (area.getX() + tw * 0.5f,
                                                         area.getRight() - tw * 0.5f, at.x),
                                           juce::jmax (12.0f, at.y - 14.0f) });
            g.setColour (juce::Colours::white);
            g.fillRoundedRectangle (pill, 7.0f);
            g.setColour (LnF::hairline());
            g.drawRoundedRectangle (pill, 7.0f, 1.0f);
            g.setColour (LnF::ink);
            g.drawText (text, pill.toNearestInt(), juce::Justification::centred, false);
        }
    }

    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (area.reduced (0.5f), 10.0f, 1.0f);
}

FxPage::EnvDisplay::Handle FxPage::EnvDisplay::handleAt (juce::Point<float> p,
                                                         const Nodes& n) const
{
    struct Candidate { juce::Point<float> at; Handle handle; };
    const Candidate candidates[] = {
        { n.a, Handle::attack },
        { n.h, Handle::hold },
        { n.d, Handle::decaySustain },
        { n.r, Handle::release },
    };
    auto best = Handle::none;
    auto bestDist = 12.0f;
    for (const auto& c : candidates)
    {
        const auto d = p.getDistanceFrom (c.at);
        if (d < bestDist)
        {
            bestDist = d;
            best = c.handle;
        }
    }
    return best;
}

void FxPage::EnvDisplay::mouseMove (const juce::MouseEvent& e)
{
    const auto n = computeNodes (owner.settings());
    const auto newHover = handleAt (e.position, n);
    if (newHover != hover)
    {
        hover = newHover;
        repaint();
    }
    setMouseCursor (newHover == Handle::none ? juce::MouseCursor::NormalCursor
                                             : juce::MouseCursor::DraggingHandCursor);
}

void FxPage::EnvDisplay::mouseExit (const juce::MouseEvent&)
{
    hover = Handle::none;
    repaint();
}

void FxPage::EnvDisplay::mouseDown (const juce::MouseEvent& e)
{
    drag = handleAt (e.position, computeNodes (owner.settings()));
    repaint();
}

void FxPage::EnvDisplay::applyDrag (const juce::MouseEvent& e)
{
    auto s = owner.settings();
    const auto n = computeNodes (s);
    const auto w = n.area.getWidth();

    switch (drag)
    {
        case Handle::attack:
            s.attackMs = static_cast<float> (juce::jlimit (0.0, 500.0,
                             (e.position.x - n.area.getX()) / (w * 0.22) * 500.0));
            break;
        case Handle::hold:
            s.holdMs = static_cast<float> (juce::jlimit (0.0, 500.0,
                           (e.position.x - n.a.x) / (w * 0.16) * 500.0));
            break;
        case Handle::decaySustain:
            s.decayMs = static_cast<float> (juce::jlimit (5.0, 1000.0,
                            (e.position.x - n.h.x) / (w * 0.22) * 1000.0));
            s.sustainPct = static_cast<float> (juce::jlimit (0.0, 100.0,
                               (n.area.getBottom() - e.position.y) / n.area.getHeight() * 100.0));
            break;
        case Handle::release:
            s.releaseMs = static_cast<float> (juce::jlimit (5.0, 500.0,
                              (e.position.x - n.s.x) / (w * 0.24) * 500.0));
            break;
        case Handle::none:
            return;
    }
    owner.setSettings (s);
}

void FxPage::EnvDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (drag != Handle::none)
        applyDrag (e);
}

void FxPage::EnvDisplay::mouseUp (const juce::MouseEvent&)
{
    drag = Handle::none;
    repaint();
}

} // namespace chopline::ui
