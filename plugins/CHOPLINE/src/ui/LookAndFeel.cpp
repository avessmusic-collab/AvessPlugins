// CHOPLINE — LookAndFeel implementation (v17 wireframe-light skin).

#include "LookAndFeel.h"

namespace chopline::ui
{

const juce::Colour ChoplineLookAndFeel::page         { 0xffdcdcde };
const juce::Colour ChoplineLookAndFeel::chassis      { 0xffe8e8ea };
const juce::Colour ChoplineLookAndFeel::paper        { 0xfff7f7f8 };
const juce::Colour ChoplineLookAndFeel::cell         { 0xfffdfdfd };
const juce::Colour ChoplineLookAndFeel::display      { 0xfffcfcfd };
const juce::Colour ChoplineLookAndFeel::ink          { 0xff0d0d0f };
const juce::Colour ChoplineLookAndFeel::label        { 0xff2a2a2e };
const juce::Colour ChoplineLookAndFeel::textDim      { 0xff48484c };
const juce::Colour ChoplineLookAndFeel::textTertiary { 0xffa4a4aa };
const juce::Colour ChoplineLookAndFeel::accent       { 0xff3a6df0 };
const juce::Colour ChoplineLookAndFeel::repeat       { 0xffff5a4f };

juce::Colour ChoplineLookAndFeel::chopShade (int chopIndex)
{
    // v17 chop_shades_on_displays (greyscale identity, 8 distinct shades).
    static const juce::Colour shades[] = {
        juce::Colour (0xff0d0d0f), juce::Colour (0xff7a7a7a),
        juce::Colour (0xff2e2e2e), juce::Colour (0xff969696),
        juce::Colour (0xff4a4a4a), juce::Colour (0xffaaaaaa),
        juce::Colour (0xff1c1c1c), juce::Colour (0xff868686),
    };
    return shades[static_cast<size_t> (((chopIndex % 8) + 8) % 8)];
}

juce::Font ChoplineLookAndFeel::uiFont (float height)
{
    return juce::Font (juce::FontOptions (height));
}

juce::Font ChoplineLookAndFeel::labelFont (float height)
{
    auto f = juce::Font (juce::FontOptions (height).withStyle ("Bold"));
    f.setExtraKerningFactor (0.14f); // wide-tracked uppercase
    return f;
}

juce::Font ChoplineLookAndFeel::monoFont (float height)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          height, juce::Font::plain));
}

ChoplineLookAndFeel::ChoplineLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, page);
    setColour (juce::Label::textColourId, label);
    setColour (juce::TextButton::buttonColourId, cell);
    setColour (juce::TextButton::textColourOffId, label);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::ComboBox::backgroundColourId, cell);
    setColour (juce::ComboBox::textColourId, ink);
    setColour (juce::ComboBox::outlineColourId, hairline());
    setColour (juce::ComboBox::arrowColourId, textDim);
    setColour (juce::PopupMenu::backgroundColourId, cell);
    setColour (juce::PopupMenu::textColourId, ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, ink);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::Slider::textBoxTextColourId, ink);
    // Knob value-arc/notch accent; a control overrides it per-slider for the
    // Beat Repeat red (v17: red is strictly the BR accent).
    setColour (juce::Slider::rotarySliderFillColourId, ink);
    // Toggled TextButton fill; the BR HOLD button overrides it to red.
    setColour (juce::TextButton::buttonOnColourId, ink);
    setColour (juce::TooltipWindow::backgroundColourId, cell);
    setColour (juce::TooltipWindow::textColourId, label);
    setColour (juce::TooltipWindow::outlineColourId, hairline());
}

// ---- knobs --------------------------------------------------------------------

void ChoplineLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float rotaryStartAngle,
                                            float rotaryEndAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto radius = size * 0.5f - 3.0f;
    const auto centre = bounds.getCentre();
    const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const auto enabled = slider.isEnabled();

    // Track arc.
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius + 2.0f, radius + 2.0f, 0.0f,
                         rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (knobTrack());
    g.strokePath (track, juce::PathStrokeType (1.4f));

    // Value arc (accent colour; BR knobs set it to the repeat red).
    const auto accentColour = slider.findColour (juce::Slider::rotarySliderFillColourId);
    if (enabled && sliderPos > 0.001f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, radius + 2.0f, radius + 2.0f, 0.0f,
                             rotaryStartAngle, angle, true);
        g.setColour (accentColour);
        g.strokePath (value, juce::PathStrokeType (2.0f));
    }

    // Flat white disc.
    const auto face = juce::Rectangle<float> (radius * 2.0f - 4.0f, radius * 2.0f - 4.0f)
                          .withCentre (centre);
    g.setColour (cell);
    g.fillEllipse (face);
    g.setColour (enabled ? knobOutline() : hairline());
    g.drawEllipse (face, 1.1f);

    // Ink notch.
    const auto inner = (radius - 2.0f) * 0.35f;
    const auto outer = (radius - 2.0f) * 0.88f;
    const juce::Point<float> p1 (centre.x + inner * std::sin (angle),
                                 centre.y - inner * std::cos (angle));
    const juce::Point<float> p2 (centre.x + outer * std::sin (angle),
                                 centre.y - outer * std::cos (angle));
    g.setColour (enabled ? accentColour : textTertiary);
    g.drawLine ({ p1, p2 }, 2.2f);
}

// ---- toggles (iOS-style mini switch, label to the left) --------------------------

void ChoplineLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                            bool highlighted, bool down)
{
    juce::ignoreUnused (down);
    const auto bounds = button.getLocalBounds();
    const auto on = button.getToggleState();

    // Switch, right-aligned: 24 x 14.
    auto sw = juce::Rectangle<float> (24.0f, 14.0f)
                  .withCentre ({ static_cast<float> (bounds.getRight()) - 14.0f,
                                 static_cast<float> (bounds.getCentreY()) });
    g.setColour (on ? ink : juce::Colour (0xffc4c4c9));
    g.fillRoundedRectangle (sw, 7.0f);
    if (highlighted)
    {
        g.setColour (juce::Colours::black.withAlpha (0.08f));
        g.fillRoundedRectangle (sw, 7.0f);
    }
    const auto thumbX = on ? sw.getRight() - 12.0f : sw.getX() + 2.0f;
    g.setColour (juce::Colours::white);
    g.fillEllipse (thumbX, sw.getY() + 2.0f, 10.0f, 10.0f);

    // Tiny uppercase label.
    g.setColour (button.isEnabled() ? label : textTertiary);
    g.setFont (labelFont (8.5f));
    g.drawText (button.getButtonText().toUpperCase(),
                bounds.withTrimmedRight (34), juce::Justification::centredLeft, false);
}

// ---- buttons ----------------------------------------------------------------------

void ChoplineLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                                const juce::Colour&, bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto radius = juce::jmin (8.0f, bounds.getHeight() * 0.5f);

    const auto active = button.getToggleState();
    const auto onColour = button.findColour (juce::TextButton::buttonOnColourId);
    auto fill = active ? onColour : cell;
    if (down)
        fill = active ? onColour.brighter (0.2f) : juce::Colour (0xffededf0);
    else if (highlighted)
        fill = active ? onColour : juce::Colour (0xfff2f2f4);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (hairline());
    g.drawRoundedRectangle (bounds, radius, 1.0f);
}

juce::Font ChoplineLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return labelFont (juce::jmin (9.5f, static_cast<float> (buttonHeight) * 0.45f));
}

void ChoplineLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                          bool, bool)
{
    g.setFont (getTextButtonFont (button, button.getHeight()));
    const auto active = button.getToggleState();
    g.setColour (! button.isEnabled() ? textTertiary
                                      : (active ? juce::Colours::white : label));
    g.drawText (button.getButtonText().toUpperCase(), button.getLocalBounds(),
                juce::Justification::centred, false);
}

// ---- combo boxes ---------------------------------------------------------------------

void ChoplineLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                        int, int, int, int, juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    const auto radius = juce::jmin (8.0f, bounds.getHeight() * 0.5f);

    g.setColour (box.isEnabled() ? cell : paper);
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (hairline());
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    // Optional tiny uppercase prefix: the combo's component NAME (v17 "SLICE …").
    if (box.getName().isNotEmpty())
    {
        g.setColour (textTertiary);
        g.setFont (labelFont (8.0f));
        g.drawText (box.getName().toUpperCase(),
                    juce::Rectangle<int> (10, 0, width - 20, height),
                    juce::Justification::centredLeft, false);
    }

    // Caret.
    const auto cx = static_cast<float> (width) - 14.0f;
    const auto cy = static_cast<float> (height) * 0.5f;
    juce::Path caret;
    caret.startNewSubPath (cx - 3.5f, cy - 2.0f);
    caret.lineTo (cx, cy + 2.0f);
    caret.lineTo (cx + 3.5f, cy - 2.0f);
    g.setColour (box.isEnabled() ? textDim : textTertiary);
    g.strokePath (caret, juce::PathStrokeType (1.2f));
}

juce::Font ChoplineLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return uiFont (11.0f);
}

void ChoplineLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& labelToPosition)
{
    const auto prefixWidth = box.getName().isNotEmpty()
        ? juce::GlyphArrangement::getStringWidthInt (labelFont (8.0f), box.getName().toUpperCase()) + 16
        : 8;
    labelToPosition.setBounds (prefixWidth, 1, box.getWidth() - prefixWidth - 18, box.getHeight() - 2);
    labelToPosition.setFont (getComboBoxFont (box));
    labelToPosition.setJustificationType (juce::Justification::centredRight);
    labelToPosition.setColour (juce::Label::textColourId, box.isEnabled() ? ink : textTertiary);
}

// ---- popup menus -----------------------------------------------------------------------

juce::Font ChoplineLookAndFeel::getPopupMenuFont()
{
    return uiFont (11.0f);
}

void ChoplineLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (cell);
    g.setColour (hairline());
    g.drawRect (juce::Rectangle<int> (0, 0, width, height), 1);
}

// ---- callouts (flyout cards) --------------------------------------------------------------

void ChoplineLookAndFeel::drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g,
                                                    const juce::Path& path, juce::Image&)
{
    g.setColour (juce::Colours::black.withAlpha (0.06f));
    g.fillPath (path, juce::AffineTransform::translation (0.0f, 1.0f));
    g.setColour (juce::Colour (0xfffafafa));
    g.fillPath (path);
    g.setColour (hairline());
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

// ---- labels ----------------------------------------------------------------------------------

void ChoplineLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& l)
{
    if (l.isBeingEdited())
        return;
    g.fillAll (l.findColour (juce::Label::backgroundColourId));
    g.setColour (l.findColour (juce::Label::textColourId)
                     .withMultipliedAlpha (l.isEnabled() ? 1.0f : 0.5f));
    g.setFont (l.getFont());
    g.drawText (l.getText(), l.getBorderSize().subtractedFrom (l.getLocalBounds()),
                l.getJustificationType(), false);
}

} // namespace chopline::ui
