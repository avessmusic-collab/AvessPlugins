#pragma once

#include <functional>

#include "CustomLookAndFeel.h"

// Rotary knob for TENSION (hero, R=74) and MIX / OUTPUT (compact, R=20).
// A juce::Slider subclass so Phase 4.2 can attach it with
// juce::SliderParameterAttachment; all drawing is custom (9-layer "knob_3d"
// build from the v4 yaml). Relative vertical drag at 0.005 normalised/px
// (setMouseDragSensitivity(200)); JUCE's Slider already applies 0.1x on Shift.
class TensionKnob : public juce::Slider
{
public:
    enum class Kind { Hero, Compact };

    TensionKnob (const TensionUI::ThemeState& themeState, Kind kindToUse,
                 juce::String labelText, std::function<juce::String (double)> readoutFn,
                 double minValue, double maxValue, double defaultValue);

    void paint (juce::Graphics&) override;

    // Component bounds for a knob whose dial centre must land at `centre`
    // (editor coordinates). Hero: 220x220. Compact: 80x90 (label below).
    static juce::Rectangle<int> boundsForCentre (Kind kind, juce::Point<int> centre);

    juce::Point<float> getDialCentre() const;

private:
    void drawKnobBody (juce::Graphics&, float cx, float cy) const;
    void drawIndicator (juce::Graphics&, float cx, float cy, float proportion,
                        juce::Colour inlay) const;

    const TensionUI::ThemeState& theme;
    Kind kind;
    juce::String label;
    std::function<juce::String (double)> readout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TensionKnob)
};
