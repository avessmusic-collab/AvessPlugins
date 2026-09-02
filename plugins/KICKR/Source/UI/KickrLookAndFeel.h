#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace kickr
{
    /**
        KICKR palette — sampled verbatim from the approved v2 mockup `:root`
        (`plugins/KICKR/.ideas/mockups/v1-ui-concept.html`). Soft slate-grey chassis,
        near-black wells, violet -> magenta signature, red reserved for energy, a lifted
        blue-violet tag for the low-frequency family.
    */
    namespace palette
    {
        inline const juce::Colour voidBlack  { 0xff08090E };
        inline const juce::Colour chassisHi  { 0xff3C414D };
        inline const juce::Colour chassisMid { 0xff31353F };
        inline const juce::Colour chassisLo  { 0xff272B34 };
        inline const juce::Colour faceplateHi { 0xff20222B };
        inline const juce::Colour faceplateLo { 0xff181A21 };
        inline const juce::Colour panelHi    { 0xff2A2E39 };
        inline const juce::Colour panelLo    { 0xff1F222B };
        inline const juce::Colour panelEdge  { 0xff3B404C };
        inline const juce::Colour well       { 0xff0A0810 };
        inline const juce::Colour wellEdge   { 0xff22252E };

        inline const juce::Colour violet     { 0xff452CDD };   // primary
        inline const juce::Colour lowfam     { 0xff6F7BFF };   // low-end family
        inline const juce::Colour magenta    { 0xffC041DD };   // LCD / crush / morph
        inline const juce::Colour red        { 0xffD1363A };   // energy

        inline const juce::Colour ink        { 0xff9EA0AE };   // labels
        inline const juce::Colour inkDim     { 0xff6B6678 };
        inline const juce::Colour inkHi      { 0xffECEAF2 };   // values / titles
    }

    /**
        Stage 3 Phase 3.1 — premium dark native LookAndFeel shared by KickrKnob /
        KickrToggle and the section combo boxes. Matte domed knob caps in moulded
        recesses with a thin ~272 degree travel arc; pill/segment toggles; LCD-style
        combo wells. No external fonts — the mockup's web fonts are approximated with
        weight + tracking on the JUCE default face (juce::FontOptions only).
    */
    class KickrLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        KickrLookAndFeel();

        void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPosProportional,
                               float rotaryStartAngle, float rotaryEndAngle,
                               juce::Slider&) override;

        void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

        void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                           int buttonX, int buttonY, int buttonW, int buttonH,
                           juce::ComboBox&) override;
        juce::Font getComboBoxFont (juce::ComboBox&) override;
        void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

        juce::Font getLabelFont (juce::Label&) override;

        void drawButtonBackground (juce::Graphics&, juce::Button&,
                                   const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

        /** 2026-09-02 (user request: "make the arrows that swap presets identical"): a
            TextButton whose properties carry "arrowDir" (-1 = left, +1 = right) gets a
            vector triangle instead of text — the two arrows are the same path mirrored,
            so they can never differ (the old "\u25C4"/"\u25BA" glyphs had different
            advance widths and the right one was being truncated to "..."). */
        void drawButtonText (juce::Graphics&, juce::TextButton&,
                             bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        /** Bold + tracked caps for section titles / wordmark. */
        static juce::Font titleFont (float heightPx);
        /** Regular tracked caps for control captions / micro labels. */
        static juce::Font microFont (float heightPx);

        /** 2026-09-01 (user request): "numbers and letters appear small" — a small,
            uniform size boost applied everywhere text is drawn (titleFont/microFont/
            combo box font here, plus KickrKnob's readout and the analyzer axis labels,
            which draw fonts directly rather than through these helpers). One constant
            so every text element grows by the same modest amount. */
        static constexpr float kTextSizeBoost = 1.12f;
    };
}
