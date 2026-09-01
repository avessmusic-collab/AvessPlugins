#include <cmath>

#include "UI/KickrKnob.h"
#include "UI/KickrLookAndFeel.h"
#include "Parameters/ParameterDescriptions.h"

namespace kickr
{
    namespace
    {
        // ~272 degree travel, symmetric about 12 o'clock (clockwise from 12 o'clock).
        constexpr float kRotaryStart = 3.926991f;               // 225 degrees
        constexpr float kRotaryEnd   = 3.926991f + 4.747295f;   // + 272 degrees
    }

    KickrKnob::KickrKnob (juce::AudioProcessorValueTreeState& apvts,
                          juce::StringRef paramID,
                          const juce::String& caption,
                          juce::Colour accent,
                          bool bipolar,
                          Size size)
        : knobSize (size)
    {
        param = apvts.getParameter (paramID);
        jassert (param != nullptr);

        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setScrollWheelEnabled (true);
        slider.setRotaryParameters (kRotaryStart, kRotaryEnd, true);
        slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
        slider.getProperties().set ("bipolar", bipolar);

        if (param != nullptr)
        {
            const auto realDefault = (double) param->convertFrom0to1 (param->getDefaultValue());
            slider.setDoubleClickReturnValue (true, realDefault);

            const auto& desc = paramDescription (paramID);
            slider.setTooltip (desc.isNotEmpty() ? desc : param->getName (64));
        }

        slider.onValueChange = [this] { refreshReadout(); repaint(); };
        addAndMakeVisible (slider);

        captionLabel.setText (caption, juce::dontSendNotification);
        captionLabel.setJustificationType (juce::Justification::centred);
        captionLabel.setInterceptsMouseClicks (false, false);
        captionLabel.setColour (juce::Label::textColourId, palette::inkDim);
        captionLabel.setBorderSize (juce::BorderSize<int> (0, 2, 0, 2));   // band height == font em-box, no slack
        addAndMakeVisible (captionLabel);

        if (param != nullptr)
            attachment = std::make_unique<juce::SliderParameterAttachment> (*param, slider, apvts.undoManager);

        refreshReadout();
    }

    void KickrKnob::refreshReadout()
    {
        if (param == nullptr)
            return;

        const float norm = param->convertTo0to1 ((float) slider.getValue());
        readoutText = param->getText (norm, 24);

        const auto unit = param->getLabel();
        if (unit.isNotEmpty())
            readoutText << " " << unit;
    }

    // 2026-09-01 (user: "make so the letters and numbers dont touch, make the numbers
    // a bit smaller"): the caption / readout bands used to be a fraction of the
    // component height (h/7 and h/6, clamped to >= 11 px) with NO gap between them,
    // while the fonts were fixed per `Size`. On the ~83 px-tall hero knobs the Large
    // caption font (12 * 1.12 = 13.4 px) overflowed its 11 px band and the readout
    // (14 px) filled its band edge-to-edge, so "0.500" collided with "MACRO", "A1"
    // with "NOTE". Both bands are now sized FROM their font (em-box height, rounded
    // up) with a fixed gap between them, the readout base size is trimmed ~10%
    // (captions keep their size — only the numbers shrink), and the caption label's
    // default 1 px top/bottom border is removed so its text really does sit inside
    // its band. Band totals stay within ~2 px of the old ones, so knob diameters are
    // essentially unchanged.
    float KickrKnob::captionFontPx() const
    {
        const float base = knobSize == Size::Large ? 12.0f
                         : knobSize == Size::Small ? 8.5f : 10.0f;
        return base * KickrLookAndFeel::kTextSizeBoost;
    }

    float KickrKnob::readoutFontPx() const
    {
        const float base = knobSize == Size::Large ? 11.0f
                         : knobSize == Size::Small ? 8.5f : 9.5f;
        return base * KickrLookAndFeel::kTextSizeBoost;
    }

    int KickrKnob::captionBandHeight() const { return (int) std::ceil (captionFontPx()); }
    int KickrKnob::readoutBandHeight() const { return (int) std::ceil (readoutFontPx()); }

    void KickrKnob::resized()
    {
        auto r = getLocalBounds();

        r.removeFromBottom (readoutBandHeight() + kCaptionReadoutGap);
        captionLabel.setBounds (r.removeFromBottom (captionBandHeight()));

        const int d = juce::jmax (8, juce::jmin (r.getWidth(), r.getHeight()));
        slider.setBounds (r.withSizeKeepingCentre (d, d));

        // microFont() applies kTextSizeBoost itself — hand it the un-boosted size.
        captionLabel.setFont (KickrLookAndFeel::microFont (captionFontPx() / KickrLookAndFeel::kTextSizeBoost));
    }

    void KickrKnob::paint (juce::Graphics& g)
    {
        const auto area = getLocalBounds().removeFromBottom (readoutBandHeight());

        g.setColour (isEnabled() ? palette::inkHi : palette::inkDim.withAlpha (0.5f));
        g.setFont (juce::Font (juce::FontOptions (readoutFontPx())));
        g.drawText (readoutText, area, juce::Justification::centred, false);
    }

    void KickrKnob::enablementChanged()
    {
        captionLabel.setColour (juce::Label::textColourId,
                                isEnabled() ? palette::inkDim : palette::inkDim.withAlpha (0.4f));
        repaint();
    }

    //==============================================================================
    KickrToggle::KickrToggle (juce::AudioProcessorValueTreeState& apvts,
                              juce::StringRef paramID,
                              const juce::String& text,
                              juce::Colour accent)
    {
        button.setButtonText (text);
        button.setClickingTogglesState (true);
        button.setColour (juce::ToggleButton::tickColourId, accent);
        addAndMakeVisible (button);

        if (auto* p = apvts.getParameter (paramID))
        {
            const auto& desc = paramDescription (paramID);
            button.setTooltip (desc.isNotEmpty() ? desc : p->getName (64));
            attachment = std::make_unique<juce::ButtonParameterAttachment> (*p, button, apvts.undoManager);
        }
    }

    void KickrToggle::resized()
    {
        button.setBounds (getLocalBounds());
    }
}
