#include "ControlWidgets.h"

using namespace TensionUI;

namespace
{
    const char* const kModeNames[3] = { "SLOW BURN", "CINEMATIC", "AGGRESSIVE" };
}

// ============================================================ RiseModeSelector
RiseModeSelector::RiseModeSelector (const ThemeState& themeState) : theme (themeState)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setBufferedToImage (true);
}

void RiseModeSelector::setSelectedIndex (int index, bool notify)
{
    index = juce::jlimit (0, 2, index);
    if (index == selected)
        return;
    selected = index;
    repaint();
    if (notify && onChange)
        onChange (selected);
}

void RiseModeSelector::mouseDown (const juce::MouseEvent& e)
{
    if (e.y < 16)
        return;
    const float segW = 220.0f / 3.0f;
    setSelectedIndex ((int) (e.position.x / segW), true);
}

void RiseModeSelector::paint (juce::Graphics& g)
{
    const auto& th = theme.current();

    // caption (baseline ~ y=436 on the canvas)
    g.setColour (Palette::textSecondary);
    g.setFont (makeFont (10.0f, true, 2.0f));
    g.drawSingleLineText ("RISE MODE", getWidth() / 2, 12, juce::Justification::horizontallyCentred);

    const juce::Rectangle<float> pill (0.0f, 16.0f, 220.0f, 30.0f);
    g.setColour (Palette::selectorBg);
    g.fillRoundedRectangle (pill, 15.0f);
    g.setColour (Palette::hairline);
    g.drawRoundedRectangle (pill.reduced (0.5f), 14.5f, 1.0f);

    const float segW = 220.0f / 3.0f;
    g.setFont (makeFont (11.0f, true, 1.0f));
    for (int i = 0; i < 3; ++i)
    {
        const juce::Rectangle<float> seg (segW * (float) i, pill.getY(), segW, pill.getHeight());
        const bool active = (i == selected);
        if (active)
        {
            const auto p = seg.reduced (3.0f, 3.0f);
            g.setColour (th.stopLow.withAlpha (0.16f));
            g.fillRoundedRectangle (p, 12.0f);
            g.setColour (th.stopLow.withAlpha (0.7f));
            g.drawRoundedRectangle (p.reduced (0.5f), 11.5f, 1.0f);
        }
        g.setColour (active ? Palette::textPrimary : Palette::textSecondary);
        g.drawText (kModeNames[i], seg.toNearestInt(), juce::Justification::centred, false);
    }
}

// ================================================================== SyncPill
SyncPill::SyncPill (const ThemeState& themeState) : theme (themeState)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setBufferedToImage (true);
}

void SyncPill::setOn (bool newOn, bool notify)
{
    if (newOn == on)
        return;
    on = newOn;
    repaint();
    if (notify && onChange)
        onChange (on);
}

void SyncPill::mouseDown (const juce::MouseEvent&) { setOn (! on, true); }

void SyncPill::paint (juce::Graphics& g)
{
    const auto& th = theme.current();
    const juce::Rectangle<float> pill (0.0f, 0.0f, 72.0f, 26.0f);
    const float ledX = 16.0f, ledY = 13.0f;

    if (on)
    {
        g.setColour (th.stopLow.withAlpha (0.14f));
        g.fillRoundedRectangle (pill, 13.0f);
        g.setColour (th.stopLow);
        g.drawRoundedRectangle (pill.reduced (0.5f), 12.5f, 1.0f);

        g.setColour (th.stopLow.withAlpha (juce::jlimit (0.0f, 1.0f, 0.25f * th.glowK)));
        g.fillEllipse (ledX - 7.0f, ledY - 7.0f, 14.0f, 14.0f);
        g.setColour (th.stopLow);
        g.fillEllipse (ledX - 4.0f, ledY - 4.0f, 8.0f, 8.0f);
    }
    else
    {
        g.setColour (Palette::hairline);
        g.drawRoundedRectangle (pill.reduced (0.5f), 12.5f, 1.0f);
        g.setColour (Palette::textDim);
        g.fillEllipse (ledX - 4.0f, ledY - 4.0f, 8.0f, 8.0f);
    }

    g.setColour (on ? Palette::textPrimary : Palette::textDim);
    g.setFont (makeFont (10.0f, true, 1.5f));
    g.drawText ("SYNC", juce::Rectangle<int> (26, 0, 42, 26), juce::Justification::centredLeft, false);
}

// ============================================================== EnclosureBezel
EnclosureBezel::EnclosureBezel()
{
    setInterceptsMouseClicks (false, false);
    setBufferedToImage (true);
}

void EnclosureBezel::paint (juce::Graphics& g)
{
    const juce::Point<float> tl { 0.0f, 0.0f }, br { 520.0f, 520.0f };

    // outer edge
    g.setColour (juce::Colour (0xff060505));
    g.drawRoundedRectangle (0.5f, 0.5f, 519.0f, 519.0f, 14.0f, 1.0f);

    // machined band: evenodd (outer rrect minus inner window)
    {
        juce::Path band;
        band.setUsingNonZeroWinding (false);
        band.addRoundedRectangle (1.0f, 1.0f, 518.0f, 518.0f, 13.0f);
        band.addRoundedRectangle (10.0f, 10.0f, 500.0f, 500.0f, 6.0f);
        g.setGradientFill (makeLinear (tl, br, { { 0.0f, juce::Colour (0xff4d4645) },
                                                 { 0.35f, juce::Colour (0xff26211f) },
                                                 { 0.65f, juce::Colour (0xff141110) },
                                                 { 1.0f, juce::Colour (0xff2e2928) } }));
        g.fillPath (band);
    }

    // top-left edge highlight
    g.setGradientFill (makeLinear (tl, br, { { 0.0f, juce::Colours::white.withAlpha (0.22f) },
                                             { 0.5f, juce::Colours::white.withAlpha (0.0f) },
                                             { 1.0f, juce::Colours::white.withAlpha (0.05f) } }));
    g.drawRoundedRectangle (1.5f, 1.5f, 517.0f, 517.0f, 12.5f, 1.0f);

    // inner lip
    g.setGradientFill (makeLinear (tl, br, { { 0.0f, juce::Colours::black.withAlpha (0.95f) },
                                             { 0.5f, juce::Colours::black.withAlpha (0.5f) },
                                             { 1.0f, juce::Colours::white.withAlpha (0.12f) } }));
    g.drawRoundedRectangle (9.5f, 9.5f, 501.0f, 501.0f, 6.5f, 1.0f);

    // wordmark (28, 37): 13 px bold, 6 px tracking
    g.setColour (Palette::textSecondary);
    g.setFont (makeFont (13.0f, true, 6.0f));
    g.drawSingleLineText ("TENSION", 28, 37, juce::Justification::left);
}
