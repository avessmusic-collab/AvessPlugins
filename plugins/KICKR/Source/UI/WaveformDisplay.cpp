#include "UI/WaveformDisplay.h"
#include "UI/KickrLookAndFeel.h"

namespace pal = kickr::palette;

namespace kickr
{
    WaveformDisplay::WaveformDisplay (Analyzer& analyzerToUse)
        : analyzer (analyzerToUse)
    {
        setInterceptsMouseClicks (false, false);
        wave.fill (0.0f);
    }

    WaveformDisplay::~WaveformDisplay()
    {
        stopTimer();
    }

    void WaveformDisplay::visibilityChanged()
    {
        if (isVisible())
            startTimerHz (30);
        else
            stopTimer();
    }

    void WaveformDisplay::timerCallback()
    {
        if (analyzer.getWaveform (wave))
            repaint();
    }

    void WaveformDisplay::paint (juce::Graphics& g)
    {
        const auto rf = getLocalBounds().toFloat();
        if (rf.isEmpty())
            return;

        const float s      = juce::jmax (0.5f, rf.getWidth() / 1120.0f);
        const float radius = juce::jmin (18.0f, rf.getHeight() * 0.10f);

        // ---- dark radial-gradient panel -------------------------------------------
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

        const float midY = r.getCentreY();

        // ---- grid + centre line --------------------------------------------------
        g.setColour (pal::ink.withAlpha (0.06f));
        for (int k = 1; k < 6; ++k)
        {
            const int gx = juce::roundToInt (r.getX() + r.getWidth() * (float) k / 6.0f);
            g.drawVerticalLine (gx, r.getY() + 4.0f * s, r.getBottom() - 4.0f * s);
        }
        g.setColour (pal::ink.withAlpha (0.11f));
        g.drawHorizontalLine (juce::roundToInt (midY), r.getX(), r.getRight());

        // ---- build the trace ---------------------------------------------------
        const int   shown  = Analyzer::kWaveCaptureLen;
        const float ampY   = r.getHeight() * 0.40f;
        const int   stride = juce::jmax (1, shown / juce::jmax (1, (int) r.getWidth() * 2));

        tracePath.clear();
        fillPath.clear();
        bool started = false;

        for (int i = 0; i < shown; i += stride)
        {
            const float x = r.getX() + r.getWidth() * (float) i / (float) shown;
            const float v = juce::jlimit (-1.35f, 1.35f, wave[static_cast<size_t> (i)]);
            const float y = midY - v * ampY;

            if (! started)
            {
                tracePath.startNewSubPath (x, y);
                fillPath.startNewSubPath (x, midY);
                fillPath.lineTo (x, y);
                started = true;
            }
            else
            {
                tracePath.lineTo (x, y);
                fillPath.lineTo (x, y);
            }
        }

        auto grad = [&] (float alpha)
        {
            juce::ColourGradient cg (pal::red.withAlpha (alpha), r.getX(), 0.0f,
                                     pal::lowfam.withAlpha (alpha), r.getRight(), 0.0f, false);
            cg.addColour (0.12, pal::magenta.withAlpha (alpha));
            cg.addColour (0.42, pal::violet.withAlpha (alpha));
            return cg;
        };

        if (started)
        {
            fillPath.lineTo (r.getRight(), midY);
            fillPath.closeSubPath();

            g.setGradientFill (grad (0.13f));
            g.fillPath (fillPath);

            // fat low-alpha glow pass, then a crisp thin pass
            g.setGradientFill (grad (0.22f));
            g.strokePath (tracePath, juce::PathStrokeType (6.0f * s,
                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setGradientFill (grad (1.0f));
            g.strokePath (tracePath, juce::PathStrokeType (1.6f * s,
                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // ---- millisecond axis --------------------------------------------------
        const double durMs = 1000.0 * (double) shown / juce::jmax (1.0, analyzer.getSampleRate());
        const int    ah    = juce::roundToInt (12.0f * s);
        const int    aw    = juce::roundToInt (54.0f * s);
        const int    ay    = juce::roundToInt (r.getBottom() - 13.0f * s);

        g.setColour (pal::ink.withAlpha (0.34f));
        g.setFont (juce::Font (juce::FontOptions (8.0f * s)));
        g.drawText ("0 ms", juce::roundToInt (r.getX()), ay, aw, ah,
                    juce::Justification::left, false);
        g.drawText (juce::String (juce::roundToInt (durMs * 0.5)),
                    juce::roundToInt (r.getCentreX() - (float) aw * 0.5f), ay, aw, ah,
                    juce::Justification::centred, false);
        g.drawText (juce::String (juce::roundToInt (durMs)) + " ms",
                    juce::roundToInt (r.getRight() - (float) aw), ay, aw, ah,
                    juce::Justification::right, false);

        // ---- KICKR watermark, bottom-right -----------------------------------
        g.setColour (pal::ink.withAlpha (0.12f));
        g.setFont (KickrLookAndFeel::titleFont (11.0f * s));
        g.drawText ("KICKR",
                    juce::roundToInt (r.getRight() - 130.0f * s),
                    juce::roundToInt (r.getY() + 4.0f * s),
                    juce::roundToInt (124.0f * s), juce::roundToInt (16.0f * s),
                    juce::Justification::right, false);
    }
}
