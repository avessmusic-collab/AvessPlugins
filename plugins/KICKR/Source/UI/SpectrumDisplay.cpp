#include "UI/SpectrumDisplay.h"
#include "UI/KickrLookAndFeel.h"
#include "UI/SpectrumCurve.h"

#include <cmath>
#include <utility>

namespace pal = kickr::palette;

namespace kickr
{
    SpectrumDisplay::SpectrumDisplay (Analyzer& analyzerToUse, juce::AudioProcessorValueTreeState& state)
        : analyzer (analyzerToUse), apvts (state)
    {
        setInterceptsMouseClicks (false, true);   // the settings selectors need clicks; the rest passes through

        rangeBox.addItemList ({ "60 dB", "90 dB", "120 dB" }, 1);
        resBox.addItemList   ({ "Res Low", "Res Med", "Res High", "Res Max" }, 1);
        speedBox.addItemList ({ "V.Slow", "Slow", "Medium", "Fast", "V.Fast" }, 1);
        tiltBox.addItemList  ({ "Tilt 0", "Tilt 1.5", "Tilt 3", "Tilt 4.5", "Tilt 6" }, 1);   // dB/oct (tooltip)
        rangeBox.setTooltip ("Analyzer range (dB shown)");
        resBox.setTooltip   ("Analyzer resolution (FFT 2048 / 4096 / 8192 / 16384)");
        speedBox.setTooltip ("Analyzer speed - how fast peaks fall back");
        tiltBox.setTooltip  ("Analyzer tilt - dB per octave around 1 kHz (4.5 makes a natural spectrum read flat)");
        for (auto* b : { &rangeBox, &resBox, &speedBox, &tiltBox })
        {
            b->setJustificationType (juce::Justification::centred);
            b->setColour (juce::ComboBox::textColourId, pal::ink);
            b->setColour (juce::ComboBox::arrowColourId, pal::ink);
            b->onChange = [this] { applySettings (true); repaint(); };
            addAndMakeVisible (b);
        }
        loadSettings();
        applySettings (false);
    }

    void SpectrumDisplay::loadSettings()
    {
        auto& st = apvts.state;
        rangeBox.setSelectedId ((int) st.getProperty ("anRange", 2), juce::dontSendNotification);   // 90 dB
        resBox.setSelectedId   ((int) st.getProperty ("anRes",   2), juce::dontSendNotification);   // Medium
        speedBox.setSelectedId ((int) st.getProperty ("anSpeed", 3), juce::dontSendNotification);   // Medium
        tiltBox.setSelectedId  ((int) st.getProperty ("anTilt",  4), juce::dontSendNotification);   // 4.5 dB/oct
    }

    void SpectrumDisplay::applySettings (bool store)
    {
        static const float ranges[]  = { 60.0f, 90.0f, 120.0f };
        static const int   orders[]  = { 11, 12, 13, 14 };
        static const float speeds[]  = { 15.0f, 30.0f, 60.0f, 120.0f, 240.0f };   // dB/s release
        static const float tilts[]   = { 0.0f, 1.5f, 3.0f, 4.5f, 6.0f };

        const int r = juce::jlimit (1, 3, rangeBox.getSelectedId());
        const int o = juce::jlimit (1, 4, resBox.getSelectedId());
        const int p = juce::jlimit (1, 5, speedBox.getSelectedId());
        const int t = juce::jlimit (1, 5, tiltBox.getSelectedId());

        analyzer.getView().rangeDb      = ranges[r - 1];
        analyzer.getView().tiltDbPerOct = tilts[t - 1];
        analyzer.setReleaseDbPerSecond (speeds[p - 1]);
        analyzer.setFrameRate (30.0f);
        if (analyzer.getFftOrder() != orders[o - 1])
            analyzer.setResolutionOrder (orders[o - 1]);

        if (store)
        {
            auto& st = apvts.state;
            st.setProperty ("anRange", r, nullptr);
            st.setProperty ("anRes",   o, nullptr);
            st.setProperty ("anSpeed", p, nullptr);
            st.setProperty ("anTilt",  t, nullptr);
        }
    }

    void SpectrumDisplay::resized()
    {
        const float s = juce::jmax (0.5f, (float) getWidth() / 1120.0f);
        const auto sc = [s] (float v) { return juce::roundToInt (v * s); };
        // Settings row, bottom-left, clear of the axis labels.
        auto row = getLocalBounds().reduced (sc (14), sc (12)).removeFromBottom (sc (20));
        row.removeFromLeft (sc (2));
        for (auto* b : { &rangeBox, &resBox, &speedBox, &tiltBox })
        {
            b->setBounds (row.removeFromLeft (sc (82)));
            row.removeFromLeft (sc (6));
        }
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
        const float rangeDb = analyzer.getView().rangeDb;   // Pro-Q "Range"
        auto yForDb = [&] (float db)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (db + rangeDb) / rangeDb);
            return r.getBottom() - r.getHeight() * t;
        };

        // ---- faint grid ----------------------------------------------------
        for (double gridHz : { 100.0, 1000.0, 10000.0 })
        {
            if (gridHz >= nyq) continue;
            g.setColour (pal::ink.withAlpha (0.08f));
            g.drawVerticalLine (juce::roundToInt (xForHz (gridHz)), r.getY(), r.getBottom());
        }
        for (int k = 1; k < 4; ++k)
        {
            g.setColour (pal::ink.withAlpha (0.05f));
            g.drawHorizontalLine (juce::roundToInt (yForDb (-rangeDb * (float) k / 4.0f)), r.getX(), r.getRight());
        }

        // ---- magnitude curve: one energy-averaged, tilted value per pixel column ----
        const int numCols = juce::jmax (2, (int) r.getWidth());
        buildSpectrumColumns (analyzer, numCols, kMinHz, (float) nyq, columns);

        curvePath.clear();
        fillPath.clear();
        bool started = false;

        for (int i = 0; i < numCols; ++i)
        {
            const float x = r.getX() + (float) i;
            const float y = yForDb (columns[static_cast<size_t> (i)]);

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
        g.setFont (juce::Font (juce::FontOptions (8.0f * s * KickrLookAndFeel::kTextSizeBoost)));
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
