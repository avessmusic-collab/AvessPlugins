#include "UI/KickrLookAndFeel.h"

namespace kickr
{
    KickrLookAndFeel::KickrLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, palette::voidBlack);

        setColour (juce::Slider::rotarySliderFillColourId,   palette::violet);
        setColour (juce::Slider::rotarySliderOutlineColourId, palette::wellEdge);
        setColour (juce::Slider::textBoxTextColourId,        palette::inkHi);

        setColour (juce::Label::textColourId,   palette::ink);
        setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        setColour (juce::ComboBox::backgroundColourId, palette::well);
        setColour (juce::ComboBox::outlineColourId,    palette::wellEdge);
        setColour (juce::ComboBox::textColourId,       palette::magenta);
        setColour (juce::ComboBox::arrowColourId,      palette::magenta);

        setColour (juce::PopupMenu::backgroundColourId,            palette::panelLo);
        setColour (juce::PopupMenu::textColourId,                  palette::ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, palette::violet.withAlpha (0.45f));
        setColour (juce::PopupMenu::highlightedTextColourId,       palette::inkHi);

        setColour (juce::TextButton::buttonColourId,   palette::panelLo);
        setColour (juce::TextButton::buttonOnColourId, palette::red.withAlpha (0.30f));
        setColour (juce::TextButton::textColourOffId,  palette::ink);
        setColour (juce::TextButton::textColourOnId,   palette::inkHi);

        setColour (juce::ToggleButton::tickColourId, palette::violet);
        setColour (juce::ToggleButton::textColourId, palette::ink);

        setColour (juce::TooltipWindow::backgroundColourId, palette::well);
        setColour (juce::TooltipWindow::textColourId,       palette::ink);
        setColour (juce::TooltipWindow::outlineColourId,    palette::panelEdge);
    }

    juce::Font KickrLookAndFeel::titleFont (float heightPx)
    {
        return juce::Font (juce::FontOptions (juce::jmax (7.0f, heightPx * kTextSizeBoost), juce::Font::bold))
                   .withExtraKerningFactor (0.18f);
    }

    juce::Font KickrLookAndFeel::microFont (float heightPx)
    {
        return juce::Font (juce::FontOptions (juce::jmax (7.0f, heightPx * kTextSizeBoost)))
                   .withExtraKerningFactor (0.09f);
    }

    void KickrLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                             float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                             juce::Slider& slider)
    {
        const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
        const float outerR = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 1.0f;
        if (outerR <= 2.0f)
            return;

        const auto centre = area.getCentre();
        const auto accent = slider.isEnabled() ? slider.findColour (juce::Slider::rotarySliderFillColourId)
                                                : palette::inkDim.withAlpha (0.45f);
        const float capR  = outerR * 0.80f;
        const float arcR  = outerR * 0.93f;
        const float arcW  = juce::jmax (1.5f, outerR * 0.07f);
        const float toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        // moulded recess
        {
            juce::ColourGradient recess (juce::Colour (0xff3C424F),
                                         centre.x - outerR * 0.35f, centre.y - outerR * 0.45f,
                                         juce::Colour (0xff0F1218),
                                         centre.x + outerR, centre.y + outerR, true);
            recess.addColour (0.44, juce::Colour (0xff262A33));
            recess.addColour (0.78, juce::Colour (0xff161A21));
            g.setGradientFill (recess);
            g.fillEllipse (centre.x - outerR, centre.y - outerR, outerR * 2.0f, outerR * 2.0f);
        }

        // travel-arc track + fill
        {
            juce::Path track;
            track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f,
                                 rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.strokePath (track, juce::PathStrokeType (arcW, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));

            const bool bipolar = (bool) slider.getProperties().getWithDefault ("bipolar", false);
            const float fromAngle = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f
                                            : rotaryStartAngle;

            juce::Path fill;
            fill.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f,
                                juce::jmin (fromAngle, toAngle), juce::jmax (fromAngle, toAngle), true);
            g.setColour (accent);
            g.strokePath (fill, juce::PathStrokeType (arcW, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }

        // matte domed cap
        {
            juce::ColourGradient cap (juce::Colour (0xff3C424F), centre.x, centre.y - capR,
                                      juce::Colour (0xff0F1218), centre.x, centre.y + capR, false);
            cap.addColour (0.5, juce::Colour (0xff262A33));
            g.setGradientFill (cap);
            g.fillEllipse (centre.x - capR, centre.y - capR, capR * 2.0f, capR * 2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.drawEllipse (centre.x - capR, centre.y - capR, capR * 2.0f, capR * 2.0f, 1.0f);
        }

        // short pointer near the rim
        {
            const float pw = juce::jmax (2.0f, outerR * 0.10f);
            juce::Path pointer;
            pointer.addRoundedRectangle (-pw * 0.5f, -capR * 0.96f, pw, capR * 0.42f, pw * 0.5f);
            g.setColour (accent);
            g.fillPath (pointer, juce::AffineTransform::rotation (toAngle).translated (centre.x, centre.y));
        }
    }

    void KickrLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                             bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
    {
        juce::ignoreUnused (shouldDrawButtonAsDown);

        auto b = button.getLocalBounds().toFloat().reduced (1.5f);
        const bool on = button.getToggleState();
        const bool enabled = button.isEnabled();
        const auto accent = button.findColour (juce::ToggleButton::tickColourId);
        const float r = juce::jmin (b.getHeight() * 0.5f, 9.0f);

        g.setColour (on && enabled ? accent.withAlpha (0.16f) : palette::panelLo);
        g.fillRoundedRectangle (b, r);

        g.setColour (! enabled ? palette::inkDim.withAlpha (0.35f)
                    : on ? accent.withAlpha (0.90f)
                    : (shouldDrawButtonAsHighlighted ? palette::ink.withAlpha (0.5f)
                                                     : palette::panelEdge));
        g.drawRoundedRectangle (b, r, 1.2f);

        g.setColour (! enabled ? palette::inkDim.withAlpha (0.5f) : (on ? palette::inkHi : palette::inkDim));
        g.setFont (microFont (juce::jmin (b.getHeight() * 0.5f, 11.0f)));
        g.drawText (button.getButtonText(), b, juce::Justification::centred, false);
    }

    void KickrLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                                         int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                                         juce::ComboBox& box)
    {
        auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (1.0f);
        g.setColour (palette::well);
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (palette::wellEdge);
        g.drawRoundedRectangle (b, 6.0f, 1.0f);

        const auto accent = box.findColour (juce::ComboBox::textColourId);
        const float ah = (float) height * 0.20f;
        const float cy = (float) height * 0.5f;

        juce::Path arrows;
        arrows.addTriangle (5.0f, cy, 5.0f + ah, cy - ah, 5.0f + ah, cy + ah);
        arrows.addTriangle ((float) width - 5.0f, cy,
                            (float) width - 5.0f - ah, cy - ah,
                            (float) width - 5.0f - ah, cy + ah);
        g.setColour (box.isEnabled() ? accent.withAlpha (0.75f) : palette::inkDim.withAlpha (0.4f));
        g.fillPath (arrows);
    }

    juce::Font KickrLookAndFeel::getComboBoxFont (juce::ComboBox& box)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                              juce::jlimit (9.0f, 15.0f, (float) box.getHeight() * 0.5f) * kTextSizeBoost,
                                              juce::Font::plain))
                   .withExtraKerningFactor (0.04f);
    }

    void KickrLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
    {
        const int arrow = juce::jmax (8, box.getHeight() / 2);
        label.setBounds (arrow, 1, juce::jmax (0, box.getWidth() - arrow * 2), juce::jmax (0, box.getHeight() - 2));
        label.setJustificationType (juce::Justification::centred);
        label.setFont (getComboBoxFont (box));
        label.setColour (juce::Label::textColourId, box.findColour (juce::ComboBox::textColourId));
    }

    juce::Font KickrLookAndFeel::getLabelFont (juce::Label& label)
    {
        const float h = label.getFont().getHeight() > 0.0f ? label.getFont().getHeight() : 11.0f;
        return microFont (h);
    }

    void KickrLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                                 const juce::Colour& backgroundColour,
                                                 bool shouldDrawButtonAsHighlighted,
                                                 bool shouldDrawButtonAsDown)
    {
        auto b = button.getLocalBounds().toFloat().reduced (1.0f);
        const bool on = button.getToggleState();
        auto base = on ? button.findColour (juce::TextButton::buttonOnColourId) : backgroundColour;

        if (! button.isEnabled())
            base = palette::panelLo.withAlpha (0.5f);
        else if (shouldDrawButtonAsDown)
            base = base.darker (0.2f);
        else if (shouldDrawButtonAsHighlighted)
            base = base.brighter (0.12f);

        g.setColour (base);
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour ((on ? palette::red : palette::panelEdge).withAlpha (button.isEnabled() ? 0.9f : 0.35f));
        g.drawRoundedRectangle (b, 8.0f, 1.0f);
    }

    juce::Font KickrLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
    {
        return microFont (juce::jlimit (8.0f, 13.0f, (float) buttonHeight * 0.42f));
    }
}
