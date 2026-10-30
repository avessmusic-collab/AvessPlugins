#include "CustomLookAndFeel.h"

namespace TensionUI
{
    juce::ColourGradient makeLinear (juce::Point<float> p1, juce::Point<float> p2,
                                     std::initializer_list<Stop> stops)
    {
        jassert (stops.size() >= 2);
        const auto* b = stops.begin();
        const auto* e = stops.end() - 1;
        juce::ColourGradient grad (b->second, p1, e->second, p2, false);
        for (const auto* s = b + 1; s != e; ++s)
            grad.addColour ((double) s->first, s->second);
        return grad;
    }

    juce::ColourGradient makeDiag (float cx, float cy, float r, std::initializer_list<Stop> stops)
    {
        const float d = 0.70710678f * r;
        return makeLinear ({ cx - d, cy - d }, { cx + d, cy + d }, stops);
    }

    juce::ColourGradient makeRadial (float cx, float cy, float r0, float r1,
                                     std::initializer_list<Stop> stops)
    {
        jassert (stops.size() >= 2 && r1 > r0);
        const auto* b = stops.begin();
        const auto* e = stops.end() - 1;
        juce::ColourGradient grad (b->second, cx, cy, e->second, cx + r1, cy, true);

        const float p0 = r0 / r1;
        if (p0 > 0.0f)
            grad.addColour ((double) p0, b->second);

        for (const auto* s = b + 1; s != e; ++s)
            grad.addColour ((double) (p0 + (1.0f - p0) * s->first), s->second);
        return grad;
    }

    juce::Font makeFont (float size, bool bold, float letterSpacingPx)
    {
        auto opts = juce::FontOptions (size);
        if (bold)
            opts = opts.withStyle ("Bold");
        if (letterSpacingPx != 0.0f)
            opts = opts.withKerningFactor (letterSpacingPx / size);
        return juce::Font (opts);
    }

    // ---------------------------------------------------------------- themes
    const ColourTheme& themeFor (int riseMode) noexcept
    {
        static const ColourTheme slowBurn   { juce::Colour (0xffff8a3d), juce::Colour (0xffff2d2a),
                                              juce::Colour (0xffd97633), 1.0f };
        static const ColourTheme cinematic  { juce::Colour (0xff2dd4c4), juce::Colour (0xff2b52ff),
                                              juce::Colour (0xff2ab5a8), 1.15f };
        static const ColourTheme aggressive { juce::Colour (0xffb266ff), juce::Colour (0xffe62bc8),
                                              juce::Colour (0xff9855d9), 1.1f };
        switch (riseMode)
        {
            case SlowBurn:   return slowBurn;
            case Aggressive: return aggressive;
            default:         return cinematic;
        }
    }

    ColourTheme lerpTheme (const ColourTheme& a, const ColourTheme& b, float t) noexcept
    {
        return { a.stopLow.interpolatedWith (b.stopLow, t),
                 a.stopHigh.interpolatedWith (b.stopHigh, t),
                 a.secondaryInlay.interpolatedWith (b.secondaryInlay, t),
                 lerp (a.glowK, b.glowK, t) };
    }

    float intensityGamma (int riseMode) noexcept
    {
        switch (riseMode)
        {
            case SlowBurn:   return 1.25f;
            case Aggressive: return 0.75f;
            default:         return 1.0f;
        }
    }

    juce::Colour accentRamp (const ColourTheme& theme, float tension) noexcept
    {
        return theme.stopLow.interpolatedWith (theme.stopHigh, smoothstep (0.35f, 1.0f, tension));
    }

    void ThemeState::setTarget (int riseMode, bool animate)
    {
        mode = riseMode;
        to = themeFor (riseMode);
        if (! animate)
        {
            from = live = to;
            progress = 1.0;
            return;
        }
        from = live;       // from the current live (possibly mid-fade) values: no snap
        progress = 0.0;
    }

    bool ThemeState::advance (double deltaMs)
    {
        if (progress >= 1.0)
            return false;

        progress = juce::jmin (1.0, progress + deltaMs / crossfadeMs);
        const float e = smoothstep (0.0f, 1.0f, (float) progress);
        live = lerpTheme (from, to, e);
        return true;
    }

    // -------------------------------------------------------------- backdrop
    void paintBackdrop (juce::Graphics& g)
    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path window;
        window.addRoundedRectangle (10.0f, 10.0f, 500.0f, 500.0f, 6.0f);
        g.reduceClipRegion (window);

        g.setGradientFill (juce::ColourGradient (Palette::backgroundTop, 0.0f, 10.0f,
                                                 Palette::backgroundBottom, 0.0f, 510.0f, false));
        g.fillPath (window);

        // rgba(0,0,0,0.55) radial from r=180 to r=370 around (260,260)
        g.setGradientFill (makeRadial (260.0f, 260.0f, 180.0f, 370.0f,
                                       { { 0.0f, juce::Colours::black.withAlpha (0.0f) },
                                         { 1.0f, juce::Colours::black.withAlpha (0.55f) } }));
        g.fillPath (window);
    }
}

// ============================================================================
CustomLookAndFeel::CustomLookAndFeel()
{
    using namespace TensionUI;
    setColour (juce::ResizableWindow::backgroundColourId, Palette::backgroundBottom);
    setColour (juce::Label::textColourId, Palette::textPrimary);
    setColour (juce::Slider::thumbColourId, Palette::textPrimary);
}

juce::Font CustomLookAndFeel::getLabelFont (juce::Label& l)
{
    return TensionUI::makeFont (l.getFont().getHeight(), false);
}
