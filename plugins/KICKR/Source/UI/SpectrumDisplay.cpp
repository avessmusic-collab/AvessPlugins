#include "UI/SpectrumDisplay.h"
#include "UI/KickrLookAndFeel.h"

#include <cmath>
#include <utility>

namespace pal = kickr::palette;

namespace kickr
{
    SpectrumDisplay::SpectrumDisplay (Analyzer& analyzerToUse)
        : analyzer (analyzerToUse)
    {
        setInterceptsMouseClicks (false, false);
    }

    SpectrumDisplay::~SpectrumDisplay()
    {
        stopTimer();
    }

    void SpectrumDisplay::visibilityChanged()
    {
        if (isVisible())
            startTimerHz (30);
        else
            stopTimer();
    }

    void SpectrumDisplay::timerCallback()
    {
        analyzer.updateSpectrum();   // FFT / windowing / dB — message thread only
        repaint();
    }

    void SpectrumDisplay::paint (juce::Graphics& g)
    {
        const auto rf = getLocalBounds().toFloat();
        if (rf.isEmpty())
            return;

        const float s      = juce::jmax (0.5f, rf.getWidth() / 1120.0f);
        const float radius = juce::jmin (18.0f, rf.getHeight() * 0.10f);

        {
            juce::Path clip;
            clip.addRoundedRectangle (rf, radius);
            g.reduceClipRegion (clip);
        }

        juce::ColourGradient bg (juce::Colour (0xff17111F),
                                 rf.getCentreX(), rf.getY() - rf.getHeight() * 0.2f,
                                 juce::Colour (0xff060409),
                                 rf.getCentreX(), rf.getBottom(), true);
        bg.addColour (0.52, juce::Colour (0xff0A0710));
        g.setGradientFill (bg);
        g.fillRoundedRectangle (rf, radius);
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (rf.reduced (0.5f), radius, 1.4f);

        auto r = rf.reduced (14.0f * s, 12.0f * s);
        if (r.getWidth() < 8.0f || r.getHeight() < 8.0f)
            return;

        const double sr    = juce::jmax (1.0, analyzer.getSampleRate());
        const double nyq   = sr * 0.5;
        const double logLo = std::log ((double) kMinHz);
        const double logHi = std::log (juce::jmax ((double) kMinHz + 1.0, nyq));

        auto xForHz = [&] (double hz)
        {
            const double clamped = juce::jlimit ((double) kMinHz, nyq, hz);
            const double t = (std::log (clamped) - logLo) / (logHi - logLo);
            return r.getX() + r.getWidth() * (float) juce::jlimit (0.0, 1.0, t);
        };
        auto yForDb = [&] (float db)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));
            return r.getBottom() - r.getHeight() * t;
        };

        // ---- faint grid ----------------------------------------------------
        for (double gridHz : { 100.0, 1000.0, 10000.0 })
        {
            if (gridHz >= nyq) continue;
            g.setColour (pal::ink.withAlpha (0.08f));
            g.drawVerticalLine (juce::roundToInt (xForHz (gridHz)), r.getY(), r.getBottom());
        }
        for (float gridDb : { -24.0f, -48.0f, -72.0f })
        {
            g.setColour (pal::ink.withAlpha (0.05f));
            g.drawHorizontalLine (juce::roundToInt (yForDb (gridDb)), r.getX(), r.getRight());
        }

        // ---- magnitude curve --------------------------------------------
        const auto&  spec  = analyzer.getSpectrumDb();
        const int    bins  = Analyzer::kNumBins;
        const double binHz = nyq / (double) bins;

        curvePath.clear();
        fillPath.clear();
        bool started = false;

        for (int b = 1; b < bins; ++b)
        {
            const double hz = (double) b * binHz;
            if (hz < (double) kMinHz) continue;

            const float x = xForHz (hz);
            const float y = yForDb (spec[static_cast<size_t> (b)]);

            if (! started)
            {
                curvePath.startNewSubPath (x, y);
                fillPath.startNewSubPath (x, r.getBottom());
                fillPath.lineTo (x, y);
                started = true;
            }
            else
            {
                curvePath.lineTo (x, y);
                fillPath.lineTo (x, y);
            }
        }

        if (! started)
            return;

        fillPath.lineTo (r.getRight(), r.getBottom());
        fillPath.closeSubPath();

        auto grad = [&] (float alpha)
        {
            juce::ColourGradient cg (pal::red.withAlpha (alpha), r.getX(), 0.0f,
                                     pal::lowfam.withAlpha (alpha), r.getRight(), 0.0f, false);
            cg.addColour (0.12, pal::magenta.withAlpha (alpha));
            cg.addColour (0.42, pal::violet.withAlpha (alpha));
            return cg;
        };

        g.setGradientFill (grad (0.16f));
        g.fillPath (fillPath);

        g.setGradientFill (grad (0.20f));
        g.strokePath (curvePath, juce::PathStrokeType (5.0f * s,
                      juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setGradientFill (grad (1.0f));
        g.strokePath (curvePath, juce::PathStrokeType (1.6f * s,
                      juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // ---- axis labels ----------------------------------------------
        g.setColour (pal::ink.withAlpha (0.32f));
        g.setFont (juce::Font (juce::FontOptions (8.0f * s)));
        const int lh = juce::roundToInt (12.0f * s);
        const int lw = juce::roundToInt (44.0f * s);
        const int ly = juce::roundToInt (r.getBottom() - 13.0f * s);
        for (auto pr : { std::pair<double, const char*> { 100.0,   "100" },
                         std::pair<double, const char*> { 1000.0,  "1k"  },
                         std::pair<double, const char*> { 10000.0, "10k" } })
        {
            if (pr.first >= nyq) continue;
            const int lx = juce::roundToInt (xForHz (pr.first) - (float) lw * 0.5f);
            g.drawText (pr.second, lx, ly, lw, lh, juce::Justification::centred, false);
        }

        g.setColour (pal::ink.withAlpha (0.12f));
        g.setFont (KickrLookAndFeel::titleFont (11.0f * s));
        g.drawText ("KICKR",
                    juce::roundToInt (r.getRight() - 130.0f * s),
                    juce::roundToInt (r.getY() + 4.0f * s),
                    juce::roundToInt (124.0f * s), juce::roundToInt (16.0f * s),
                    juce::Justification::right, false);
    }
}
