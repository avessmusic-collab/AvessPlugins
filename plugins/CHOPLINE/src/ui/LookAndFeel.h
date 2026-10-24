// CHOPLINE — LookAndFeel (spec §9, locked v17 mockup)
// Wireframe-light skin: paper-white cards on light grey, hairline borders,
// light hairline-grid displays, flat white disc knobs with a near-black ink
// notch, tiny wide-tracked uppercase labels, mono value pills, iOS-style mini
// toggles. Blue #3a6df0 only as a THIN accent (selection / playhead); red
// #ff5a4f strictly for Beat Repeat activity (later phases). Design tokens in
// one place per spec §9.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace chopline::ui
{

class ChoplineLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    // ---- design tokens (v17) -------------------------------------------------
    static const juce::Colour page;         // window background grey #dcdcde
    static const juce::Colour chassis;      // flat plate #e8e8ea
    static const juce::Colour paper;        // card / panel #f7f7f8
    static const juce::Colour cell;         // module card #fdfdfd
    static const juce::Colour display;      // display background #fcfcfd
    static const juce::Colour ink;          // near-black strokes / text #0d0d0f
    static const juce::Colour label;        // tiny uppercase labels #2a2a2e
    static const juce::Colour textDim;      // #48484c
    static const juce::Colour textTertiary; // dim / disabled #a4a4aa
    static const juce::Colour accent;       // selection / playhead blue #3a6df0
    static const juce::Colour repeat;       // Beat Repeat red (only) #ff5a4f

    static juce::Colour hairline() { return juce::Colours::black.withAlpha (0.14f); }
    static juce::Colour gridLine() { return juce::Colours::black.withAlpha (0.10f); }
    static juce::Colour knobTrack() { return juce::Colours::black.withAlpha (0.22f); }
    static juce::Colour knobOutline() { return juce::Colours::black.withAlpha (0.55f); }

    /// Greyscale chop shades for display surfaces (v17: no colour palette).
    static juce::Colour chopShade (int chopIndex);

    // ---- fonts -----------------------------------------------------------------
    static juce::Font uiFont (float height);
    static juce::Font labelFont (float height = 8.5f); // wide-tracked uppercase
    static juce::Font monoFont (float height = 9.0f);

    ChoplineLookAndFeel();

    // ---- component drawing -------------------------------------------------------
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics&,
                                   const juce::Path&, juce::Image&) override;
    int getCallOutBoxBorderSize (const juce::CallOutBox&) override { return 12; }

    void drawLabel (juce::Graphics&, juce::Label&) override;
};

} // namespace chopline::ui
