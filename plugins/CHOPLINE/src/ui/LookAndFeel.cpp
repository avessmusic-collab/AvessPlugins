// CHOPLINE — LookAndFeel implementation (Phase 0 stub).

#include "LookAndFeel.h"

namespace chopline::ui
{

const juce::Colour ChoplineLookAndFeel::paper   { 0xfff4f4f6 };
const juce::Colour ChoplineLookAndFeel::chassis { 0xffe4e4e8 };
const juce::Colour ChoplineLookAndFeel::ink     { 0xff0d0d0f };
const juce::Colour ChoplineLookAndFeel::label   { 0xff2a2a2e };
const juce::Colour ChoplineLookAndFeel::accent  { 0xff3a6df0 };
const juce::Colour ChoplineLookAndFeel::repeat  { 0xffff5a4f };

ChoplineLookAndFeel::ChoplineLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, paper);
    setColour (juce::Label::textColourId, label);
}

} // namespace chopline::ui
