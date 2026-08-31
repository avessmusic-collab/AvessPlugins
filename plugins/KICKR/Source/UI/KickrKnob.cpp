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
            slider.setTooltip ((desc.isNotEmpty() ? desc : param->getName (64))
                               + juce::String (" - drag / wheel to adjust, double-click to reset"));
        }

        slider.onValueChange = [this] { refreshReadout(); repaint(); };
        addAndMakeVisible (slider);

        captionLabel.setText (caption, juce::dontSendNotification);
        captionLabel.setJustificationType (juce::Justification::centred);
        captionLabel.setInterceptsMouseClicks (false, false);
        captionLabel.setColour (juce::Label::textColourId, palette::inkDim);
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

    void KickrKnob::resized()
    {
        auto r = getLocalBounds();

        const int readoutH = juce::jlimit (11, 22, r.getHeight() / 6);
        const int captionH = juce::jlimit (11, 20, r.getHeight() / 7);

        r.removeFromBottom (readoutH);
        captionLabel.setBounds (r.removeFromBottom (captionH));

        const int d = juce::jmax (8, juce::jmin (r.getWidth(), r.getHeight()));
        slider.setBounds (r.withSizeKeepingCentre (d, d));

        const float capFs = knobSize == Size::Large ? 12.0f
                          : knobSize == Size::Small ? 8.5f : 10.0f;
        captionLabel.setFont (KickrLookAndFeel::microFont (capFs));
    }

    void KickrKnob::paint (juce::Graphics& g)
    {
        const int readoutH = juce::jlimit (11, 22, getHeight() / 6);
        const auto area = getLocalBounds().removeFromBottom (readoutH);

        const float fs = knobSize == Size::Large ? 12.5f
                       : knobSize == Size::Small ? 9.0f : 10.5f;

        g.setColour (palette::inkHi);
        g.setFont (juce::Font (juce::FontOptions (fs)));
        g.drawText (readoutText, area, juce::Justification::centred, false);
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
