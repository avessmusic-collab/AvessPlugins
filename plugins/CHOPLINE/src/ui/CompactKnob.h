// CHOPLINE — CompactKnob (v17)
// Small flat disc knob with a tiny wide-tracked uppercase caption that swaps
// to a mono value readout while hovered or dragged (v17 lettering rules).
// Layout: dial on the left, caption/value to the right (fits the 140x40
// strip cells); or dial on top with the caption below when `vertical`.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "LookAndFeel.h"

namespace chopline::ui
{

class CompactKnob final : public juce::Component
{
public:
    CompactKnob (const juce::String& captionToUse, bool verticalLayout = false)
        : caption (captionToUse), vertical (verticalLayout)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        slider.setPopupDisplayEnabled (false, false, nullptr);
        slider.addMouseListener (this, false);
        slider.onValueChange = [this]
        {
            if (onValueChange != nullptr)
                onValueChange (slider.getValue());
            repaint();
        };
        addAndMakeVisible (slider);
    }

    juce::Slider& getSlider() noexcept { return slider; }

    std::function<void (double)> onValueChange;
    std::function<juce::String (double)> formatValue; // default: "55%"-style via suffix
    juce::String valueSuffix;

    void setValueQuiet (double v)
    {
        slider.setValue (v, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        const auto b = getLocalBounds();
        if (vertical)
            slider.setBounds (b.withHeight (juce::jmin (b.getWidth(), b.getHeight() - 12)));
        else
            slider.setBounds (b.withWidth (juce::jmin (b.getHeight(), b.getWidth())));
    }

    void paint (juce::Graphics& g) override
    {
        const auto showValue = hovered || dragging;
        const auto text = showValue ? currentValueText() : caption.toUpperCase();

        const auto textArea = vertical
            ? getLocalBounds().withTop (getHeight() - 12)
            : getLocalBounds().withTrimmedLeft (slider.getRight() + 6);

        if (showValue)
        {
            // Mono value pill.
            g.setFont (ChoplineLookAndFeel::monoFont (9.0f));
            const auto w = juce::jmin (textArea.getWidth(),
                                       8 + static_cast<int> (std::ceil (
                                           juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text))));
            auto pill = textArea.withSizeKeepingCentre (juce::jmax (26, w), 14).toFloat();
            if (! vertical)
                pill.setX (static_cast<float> (textArea.getX()));
            g.setColour (juce::Colours::white);
            g.fillRoundedRectangle (pill, 7.0f);
            g.setColour (ChoplineLookAndFeel::hairline());
            g.drawRoundedRectangle (pill, 7.0f, 1.0f);
            g.setColour (ChoplineLookAndFeel::ink);
            g.drawText (text, pill.toNearestInt(), juce::Justification::centred, false);
        }
        else
        {
            g.setColour (isEnabled() ? ChoplineLookAndFeel::label : ChoplineLookAndFeel::textTertiary);
            g.setFont (ChoplineLookAndFeel::labelFont (8.5f));
            g.drawText (text, textArea,
                        vertical ? juce::Justification::centred : juce::Justification::centredLeft,
                        false);
        }
    }

    void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { dragging = true; repaint(); }
    void mouseUp (const juce::MouseEvent&) override { dragging = false; repaint(); }

private:
    juce::String currentValueText() const
    {
        if (formatValue != nullptr)
            return formatValue (slider.getValue());
        const auto interval = slider.getInterval();
        const auto decimals = interval >= 1.0 ? 0 : 1;
        return juce::String (slider.getValue(), decimals) + valueSuffix;
    }

    juce::Slider slider;
    juce::String caption;
    bool vertical = false;
    bool hovered = false, dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompactKnob)
};

} // namespace chopline::ui
