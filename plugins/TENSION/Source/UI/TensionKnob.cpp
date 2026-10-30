#include "TensionKnob.h"

using namespace TensionUI;

namespace
{
    constexpr float kStartDeg = 135.0f;   // screen angles: 0 = +x, clockwise
    constexpr float kSweepDeg = 270.0f;

    constexpr float deg2rad (float d) { return d * juce::MathConstants<float>::pi / 180.0f; }

    // JUCE arc angles start at 12 o'clock, clockwise.
    constexpr float toJuceAngle (float screenDeg) { return deg2rad (screenDeg + 90.0f); }

    juce::Colour black (float a) { return juce::Colours::black.withAlpha (a); }
    juce::Colour white (float a) { return juce::Colours::white.withAlpha (a); }

    void fillDisc (juce::Graphics& g, float cx, float cy, float r)
    {
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
    }

    void strokeRing (juce::Graphics& g, float cx, float cy, float r, float width)
    {
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, width);
    }
}

TensionKnob::TensionKnob (const ThemeState& themeState, Kind kindToUse, juce::String labelText,
                          std::function<juce::String (double)> readoutFn,
                          double minValue, double maxValue, double defaultValue)
    : theme (themeState), kind (kindToUse), label (std::move (labelText)),
      readout (std::move (readoutFn))
{
    setSliderStyle (juce::Slider::RotaryVerticalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    setRange (minValue, maxValue);
    setValue (defaultValue, juce::dontSendNotification);
    setDoubleClickReturnValue (true, defaultValue);
    setRotaryParameters (toJuceAngle (kStartDeg), toJuceAngle (kStartDeg + kSweepDeg), true);
    setMouseDragSensitivity (200);                 // 1 / 0.005 per px
    setSliderSnapsToMousePosition (false);         // relative drag
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setBufferedToImage (true);                     // static layers re-render only on change
}

juce::Rectangle<int> TensionKnob::boundsForCentre (Kind kind, juce::Point<int> centre)
{
    if (kind == Kind::Hero)
        return { centre.x - 110, centre.y - 110, 220, 220 };
    return { centre.x - 40, centre.y - 34, 80, 90 };
}

juce::Point<float> TensionKnob::getDialCentre() const
{
    if (kind == Kind::Hero)
        return { (float) getWidth() * 0.5f, (float) getHeight() * 0.5f };
    return { (float) getWidth() * 0.5f, 34.0f };
}

// -------------------------------------------------------------------- body
void TensionKnob::drawKnobBody (juce::Graphics& g, float cx, float cy) const
{
    const bool hero = (kind == Kind::Hero);
    const float R      = hero ? 74.0f : 20.0f;
    const float bevel  = hero ? 6.0f : 3.0f;
    const float wellR  = hero ? 83.0f : 23.0f;
    const float so     = hero ? 7.0f : 3.5f;
    const float dr     = R - bevel;

    // 1. well: recessed socket, lit lower-right lip
    g.setGradientFill (makeDiag (cx, cy, wellR, { { 0.0f, juce::Colour (0xff020202) },
                                                  { 0.5f, juce::Colour (0xff0b0a0a) },
                                                  { 1.0f, juce::Colour (0xff221d1c) } }));
    fillDisc (g, cx, cy, wellR);
    g.setGradientFill (makeDiag (cx, cy, wellR, { { 0.0f, black (0.9f) },
                                                  { 0.5f, black (0.0f) },
                                                  { 1.0f, white (0.13f) } }));
    strokeRing (g, cx, cy, wellR - 0.5f, 1.0f);

    // 2. contact shadow: offset disc clipped to the well
    {
        juce::Graphics::ScopedSaveState s (g);
        juce::Path wellClip;
        wellClip.addEllipse (cx - (wellR - 1.0f), cy - (wellR - 1.0f), (wellR - 1.0f) * 2.0f, (wellR - 1.0f) * 2.0f);
        g.reduceClipRegion (wellClip);

        const float scx = cx + 0.35f * so, scy = cy + so;
        const float outer = R + (hero ? 14.0f : 7.0f);
        g.setGradientFill (makeRadial (scx, scy, 0.85f * R, outer,
                                       { { 0.0f, black (0.8f) }, { 0.5f, black (0.4f) }, { 1.0f, black (0.0f) } }));
        fillDisc (g, scx, scy, outer);
    }

    // 3. bevel skirt
    g.setGradientFill (makeDiag (cx, cy, R, { { 0.0f, juce::Colour (0xff6f6867) },
                                              { 0.42f, juce::Colour (0xff2c2827) },
                                              { 0.58f, juce::Colour (0xff141111) },
                                              { 1.0f, juce::Colour (0xff050404) } }));
    fillDisc (g, cx, cy, R);

    // 4. dome. JUCE has no two-point conical gradient: approximated by a radial
    // gradient centred on the focal point, radius chosen so the far edge lands
    // on the last stop.
    {
        const float fx = cx - 0.3f * dr, fy = cy - 0.4f * dr;
        g.setGradientFill (makeRadial (fx, fy, 0.0f, 1.45f * dr,
                                       { { 0.0f, juce::Colour (0xff4f4847) },
                                         { 0.3f, juce::Colour (0xff2f2a29) },
                                         { 0.7f, juce::Colour (0xff131111) },
                                         { 1.0f, juce::Colour (0xff070606) } }));
        fillDisc (g, cx, cy, dr);
    }

    {
        juce::Graphics::ScopedSaveState s (g);
        juce::Path domeClip;
        domeClip.addEllipse (cx - dr, cy - dr, dr * 2.0f, dr * 2.0f);
        g.reduceClipRegion (domeClip);

        // 5. machined concentric rings (hero only)
        if (hero)
        {
            g.setColour (white (0.03f));
            for (float f : { 0.3f, 0.5f, 0.7f, 0.88f })
                strokeRing (g, cx, cy, dr * f, 0.8f);
        }

        // 6. bounce light from lower-right
        {
            const float bx = cx + 0.5f * dr, by = cy + 0.6f * dr, br = 0.6f * dr;
            g.setGradientFill (makeRadial (bx, by, 0.0f, br, { { 0.0f, white (0.07f) }, { 1.0f, white (0.0f) } }));
            fillDisc (g, bx, by, br);
        }

        // 7. inner edge shadow (darkens lower-right inside the dome)
        g.setGradientFill (makeDiag (cx, cy, dr, { { 0.0f, black (0.0f) }, { 0.4f, black (0.0f) }, { 1.0f, black (0.6f) } }));
        strokeRing (g, cx, cy, dr - 1.5f, 3.0f);
    }

    // 8. rim line (outside the clip)
    g.setGradientFill (makeDiag (cx, cy, dr, { { 0.0f, white (0.24f) }, { 0.5f, white (0.02f) }, { 1.0f, black (0.55f) } }));
    strokeRing (g, cx, cy, dr, 1.0f);

    // 9. rotated elliptical specular
    {
        juce::Graphics::ScopedSaveState s (g);
        g.addTransform (juce::AffineTransform::scale (1.0f, 0.6f)
                            .rotated (deg2rad (-35.0f))
                            .translated (cx - 0.32f * dr, cy - 0.42f * dr));
        const float sr = 0.34f * dr;
        g.setGradientFill (makeRadial (0.0f, 0.0f, 0.0f, sr,
                                       { { 0.0f, white (0.30f) }, { 0.5f, white (0.10f) }, { 1.0f, white (0.0f) } }));
        fillDisc (g, 0.0f, 0.0f, sr);
    }
}

void TensionKnob::drawIndicator (juce::Graphics& g, float cx, float cy, float proportion,
                                 juce::Colour inlay) const
{
    const bool hero = (kind == Kind::Hero);
    const float a = deg2rad (kStartDeg + kSweepDeg * proportion);
    const float ca = std::cos (a), sa = std::sin (a);
    const float r0 = hero ? 44.0f : 9.0f;
    const float r1 = hero ? 66.0f : 17.0f;

    juce::Path line;
    line.startNewSubPath (cx + ca * r0, cy + sa * r0);
    line.lineTo (cx + ca * r1, cy + sa * r1);

    const auto rounded = [] (float w) { return juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded); };

    g.setColour (black (0.65f));                                       // groove
    g.strokePath (line, rounded (hero ? 5.0f : 3.4f));

    const float lip = hero ? 1.6f : 1.0f;                              // lit lower-right wall
    g.setColour (white (0.10f));
    g.strokePath (line, juce::PathStrokeType (1.0f), juce::AffineTransform::translation (lip, lip));

    g.setColour (inlay);                                               // inlay
    g.strokePath (line, rounded (hero ? 3.0f : 2.0f));
}

// ------------------------------------------------------------------- paint
void TensionKnob::paint (juce::Graphics& g)
{
    const auto c = getDialCentre();
    const float cx = c.x, cy = c.y;
    const bool hero = (kind == Kind::Hero);
    const auto& th = theme.current();

    const float p = (float) valueToProportionOfLength (getValue());
    const float trackR = hero ? 88.0f : 25.0f;
    const float trackW = hero ? 5.0f : 3.0f;

    const float a0 = toJuceAngle (kStartDeg);
    const float a1 = toJuceAngle (kStartDeg + kSweepDeg);

    // track
    {
        juce::Path arc;
        arc.addCentredArc (cx, cy, trackR, trackR, 0.0f, a0, a1, true);
        g.setColour (Palette::track);
        g.strokePath (arc, juce::PathStrokeType (trackW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // value arc (+ glow on the hero)
    const juce::Colour accent = hero ? accentRamp (th, p) : th.stopLow.withMultipliedBrightness (0.85f);
    if (p > 0.002f)
    {
        juce::Path arc;
        arc.addCentredArc (cx, cy, trackR, trackR, 0.0f, a0, a0 + (a1 - a0) * p, true);

        if (hero)
        {
            const float i = p;                       // 4.1: intensity ~ tension (4.3 may feed the smoothed value)
            g.setColour (accent.withAlpha (juce::jlimit (0.0f, 1.0f, 0.08f * i * th.glowK)));
            g.strokePath (arc, juce::PathStrokeType (18.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (accent.withAlpha (juce::jlimit (0.0f, 1.0f, 0.18f * i * th.glowK)));
            g.strokePath (arc, juce::PathStrokeType (11.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (accent);
        g.strokePath (arc, juce::PathStrokeType (trackW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // tick ring (hero only): 41 ticks r=96..100, ticks inside the value range tinted
    if (hero)
    {
        for (int k = 0; k < 41; ++k)
        {
            const float f = (float) k / 40.0f;
            const float a = deg2rad (kStartDeg + kSweepDeg * f);
            const juce::Line<float> tick (cx + std::cos (a) * 96.0f, cy + std::sin (a) * 96.0f,
                                          cx + std::cos (a) * 100.0f, cy + std::sin (a) * 100.0f);
            const bool lit = (p > 0.002f && f <= p);
            g.setColour (lit ? accentRamp (th, f).withAlpha (0.6f) : Palette::textDim);
            g.drawLine (tick, 1.0f);
        }
    }

    drawKnobBody (g, cx, cy);

    const juce::Colour inlay = hero ? (p < 0.02f ? Palette::textPrimary : accent) : th.secondaryInlay;
    drawIndicator (g, cx, cy, p, inlay);

    // readout + label
    const auto text = readout ? readout (getValue()) : juce::String (getValue(), 1);
    if (hero)
    {
        g.setColour (Palette::textPrimary);
        g.setFont (makeFont (40.0f, true));
        g.drawSingleLineText (text, juce::roundToInt (cx), juce::roundToInt (cy + 16.0f), juce::Justification::horizontallyCentred);

        g.setColour (Palette::textSecondary);
        g.setFont (makeFont (10.0f, true, 2.0f));
        g.drawSingleLineText (label, juce::roundToInt (cx), juce::roundToInt (cy + 46.0f), juce::Justification::horizontallyCentred);
    }
    else
    {
        g.setColour (Palette::textPrimary);
        g.setFont (makeFont (11.0f, false));
        g.drawSingleLineText (text, juce::roundToInt (cx), juce::roundToInt (cy + 7.0f), juce::Justification::horizontallyCentred);

        g.setColour (Palette::textSecondary);
        g.setFont (makeFont (10.0f, true, 2.0f));
        g.drawSingleLineText (label, juce::roundToInt (cx), juce::roundToInt (cy + 44.0f), juce::Justification::horizontallyCentred);
    }
}
