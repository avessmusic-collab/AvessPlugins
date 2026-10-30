#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <initializer_list>
#include <utility>

// TENSION native GUI - shared look: palette, mode colour themes (+ 450 ms
// crossfade machinery), gradient helpers, typography and the near-black
// backdrop. Locked design contract: .ideas/mockups/v4-ui.yaml (520x520 canvas).

namespace TensionUI
{
    // ---------------------------------------------------------------- palette
    namespace Palette
    {
        inline const juce::Colour backgroundTop    { 0xff0d0b0c };
        inline const juce::Colour backgroundBottom { 0xff050405 };
        inline const juce::Colour textPrimary      { 0xffece6e1 };
        inline const juce::Colour textSecondary    { 0xff8a817b };
        inline const juce::Colour textDim          { 0xff4f4945 };
        inline const juce::Colour track            { 0xff241f1e };
        inline const juce::Colour hairline         { 0xff2d2827 };
        inline const juce::Colour selectorBg       { 0xff141111 };
    }

    // -------------------------------------------------------- canvas constants
    inline constexpr int   kWidth  = 520;
    inline constexpr int   kHeight = 520;
    inline constexpr float kFieldCentreX = 260.0f;
    inline constexpr float kFieldCentreY = 235.0f;

    // ---------------------------------------------------------------- helpers
    inline float smoothstep (float a, float b, float x) noexcept
    {
        const float t = juce::jlimit (0.0f, 1.0f, (x - a) / (b - a));
        return t * t * (3.0f - 2.0f * t);
    }

    inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }

    using Stop = std::pair<float, juce::Colour>;

    // Linear gradient between two points; stops[0] is at 0, stops.back() at 1,
    // interior stops carry their own position.
    juce::ColourGradient makeLinear (juce::Point<float> p1, juce::Point<float> p2,
                                     std::initializer_list<Stop> stops);

    // "diag(r)" from the YAML: top-left -> bottom-right across a disc of radius r.
    juce::ColourGradient makeDiag (float cx, float cy, float r, std::initializer_list<Stop> stops);

    // Radial gradient running from radius r0 to r1 around (cx,cy); stop positions
    // are expressed 0..1 across [r0, r1]. Colour is constant inside r0.
    juce::ColourGradient makeRadial (float cx, float cy, float r0, float r1,
                                     std::initializer_list<Stop> stops);

    // Font with extra letter spacing expressed in pixels.
    juce::Font makeFont (float size, bool bold, float letterSpacingPx = 0.0f);

    // ----------------------------------------------------------------- themes
    struct ColourTheme
    {
        juce::Colour stopLow;
        juce::Colour stopHigh;
        juce::Colour secondaryInlay;
        float glowK = 1.0f;
    };

    enum RiseMode { SlowBurn = 0, Cinematic = 1, Aggressive = 2 };

    const ColourTheme& themeFor (int riseMode) noexcept;
    ColourTheme lerpTheme (const ColourTheme& a, const ColourTheme& b, float t) noexcept;

    // Pulse-field intensity exponent per rise mode (v4 yaml: 1.25 / 1.0 / 0.75).
    float intensityGamma (int riseMode) noexcept;

    // accent(t) = lerp(stopLow, stopHigh, smoothstep(0.35, 1.0, tension))
    juce::Colour accentRamp (const ColourTheme& theme, float tension) noexcept;

    // Holds the live (crossfaded) theme. On a rise-mode change the stops and
    // glow_k are eased (smoothstep) from the current live values to the new
    // theme over 450 ms. Drive advance() from a 60 Hz timer (no snap).
    class ThemeState
    {
    public:
        static constexpr double crossfadeMs = 450.0;

        ThemeState() { setTarget (Cinematic, false); }

        void setTarget (int riseMode, bool animate);
        // Returns true when the live theme changed (caller should repaint).
        bool advance (double deltaMs);

        const ColourTheme& current() const noexcept { return live; }
        int  getMode() const noexcept { return mode; }
        bool isAnimating() const noexcept { return progress < 1.0; }

    private:
        ColourTheme from, to, live;
        double progress = 1.0;
        int mode = Cinematic;
    };

    // Near-black vertical gradient + vignette, clipped to the 500x500 inner
    // window (6 px corners). Pure paint, no state.
    void paintBackdrop (juce::Graphics& g);
}

// The plugin's LookAndFeel: owns the ThemeState and applies typography /
// default colours. The three hand-drawn controls (TensionKnob, RiseModeSelector,
// SyncPill) read the theme from it.
class CustomLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CustomLookAndFeel();

    TensionUI::ThemeState&       getTheme() noexcept       { return theme; }
    const TensionUI::ThemeState& getTheme() const noexcept { return theme; }

    juce::Font getLabelFont (juce::Label&) override;

private:
    TensionUI::ThemeState theme;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomLookAndFeel)
};
