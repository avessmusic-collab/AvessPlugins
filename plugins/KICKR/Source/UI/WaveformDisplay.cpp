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
            startTimerHz (60);   // ~16 ms latency — near real-time while the kick draws in
        else
            stopTimer();
    }

    void WaveformDisplay::timerCallback()
    {
        std::uint32_t gen = 0;
        const int len = analyzer.getWaveform (wave, gen);

        if (len != validLen || gen != generation)
        {
            validLen   = len;
            generation = gen;
            repaint();
        }
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

        // ---- build the trace -------------------------------------------------------
        //  The x axis is a FIXED window (kWaveCaptureLen), same scale every trigger.
        //  Capture sample 0 IS the note-on's own sample-accurate offset (dropped at the
        //  source — see Analyzer::armCapture's `skipSamples`), so drawing from i=0 here
        //  already starts exactly at the trigger with no empty space, at a constant
        //  scale, for every retrigger timing — no per-frame detection needed.
        const int   window  = Analyzer::kWaveCaptureLen;
        const int   drawTo  = juce::jlimit (0, window, validLen);
        const float ampY    = r.getHeight() * 0.40f;
        const int   stride  = juce::jmax (1, window / juce::jmax (1, (int) r.getWidth() * 2));

        tracePath.clear();
        fillPath.clear();
        bool  started = false;
        float lastX   = r.getX();

        for (int i = 0; i < drawTo; i += stride)
        {
            const float x = r.getX() + r.getWidth() * (float) i / (float) window;
            const float v = juce::jlimit (-1.35f, 1.35f, wave[static_cast<size_t> (i)]);
            const float y = midY - v * ampY;
            lastX = x;

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
            fillPath.lineTo (lastX, midY);
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

            // leading-edge dot — the "pulse" head while the kick is still drawing in
            if (drawTo < Analyzer::kWaveCaptureLen)
            {
                const float hy = midY - juce::jlimit (-1.35f, 1.35f,
                                     wave[static_cast<size_t> (juce::jmax (0, drawTo - 1))]) * ampY;
                g.setColour (pal::lowfam.withAlpha (0.9f));
                g.fillEllipse (lastX - 2.4f * s, hy - 2.4f * s, 4.8f * s, 4.8f * s);
                g.setColour (pal::lowfam.withAlpha (0.25f));
                g.fillEllipse (lastX - 5.0f * s, hy - 5.0f * s, 10.0f * s, 10.0f * s);
            }
        }

        // ---- millisecond axis --------------------------------------------------
        const double durMs = 1000.0 * (double) window / juce::jmax (1.0, analyzer.getSampleRate());
        const int    ah    = juce::roundToInt (12.0f * s);
        const int    aw    = juce::roundToInt (54.0f * s);
        const int    ay    = juce::roundToInt (r.getBottom() - 13.0f * s);

        g.setColour (pal::ink.withAlpha (0.34f));
        g.setFont (juce::Font (juce::FontOptions (8.0f * s * KickrLookAndFeel::kTextSizeBoost)));
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
