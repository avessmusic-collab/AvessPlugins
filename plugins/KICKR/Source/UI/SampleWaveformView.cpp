#include "UI/SampleWaveformView.h"
#include "UI/KickrLookAndFeel.h"
#include "Parameters/ParameterIDs.h"

namespace kickr
{
    SampleWaveformView::SampleWaveformView (juce::AudioProcessorValueTreeState& apvtsToUse)
        : apvts (apvtsToUse)
    {
        setInterceptsMouseClicks (false, false);
        peaksMin.fill (0.0f);
        peaksMax.fill (0.0f);
    }

    SampleWaveformView::~SampleWaveformView()
    {
        stopTimer();
    }

    void SampleWaveformView::visibilityChanged()
    {
        if (isVisible())
            startTimerHz (15);
        else
            stopTimer();
    }

    void SampleWaveformView::timerCallback()
    {
        if (! hasSample)
            return;

        auto* s = apvts.getRawParameterValue (id::sampleStart);
        auto* e = apvts.getRawParameterValue (id::sampleEnd);
        const float s01 = s != nullptr ? s->load() : 0.0f;
        const float e01 = e != nullptr ? e->load() : 1.0f;

        if (std::abs (s01 - lastStart01) > 1.0e-4f || std::abs (e01 - lastEnd01) > 1.0e-4f)
        {
            lastStart01 = s01;
            lastEnd01   = e01;
            repaint();
        }
    }

    void SampleWaveformView::setSample (SampleLibrary& lib, const juce::String& name)
    {
        if (name == cachedName)
            return;   // already showing this file — skip the redundant decode

        cachedName = name;
        hasSample  = false;
        peaksMin.fill (0.0f);
        peaksMax.fill (0.0f);

        if (name.isNotEmpty())
        {
            // Same decode path the processor itself uses (SampleLibrary::load()) — small
            // files (<= 5 s), synchronous on the message thread is fine. A missing/bad
            // file just leaves hasSample false (matches the "sample missing" label state).
            if (auto sb = lib.load (name))
            {
                const int n  = sb->audio.getNumSamples();
                const int ch = sb->audio.getNumChannels();

                if (n > 0)
                {
                    for (int col = 0; col < kNumPeaks; ++col)
                    {
                        const auto i0 = static_cast<int> ((static_cast<juce::int64> (col) * n) / kNumPeaks);
                        const auto i1 = juce::jmax (i0 + 1, static_cast<int> (
                                            (static_cast<juce::int64> (col + 1) * n) / kNumPeaks));
                        float mn = 0.0f, mx = 0.0f;
                        for (int i = i0; i < i1 && i < n; ++i)
                        {
                            float v = sb->audio.getSample (0, i);
                            if (ch > 1)
                                v = 0.5f * (v + sb->audio.getSample (1, i));
                            mn = juce::jmin (mn, v);
                            mx = juce::jmax (mx, v);
                        }
                        peaksMin[static_cast<size_t> (col)] = mn;
                        peaksMax[static_cast<size_t> (col)] = mx;
                    }
                    hasSample = true;
                }
            }
        }

        lastStart01 = -1.0f;   // force the trim shading to refresh on the next timer tick
        repaint();
    }

    void SampleWaveformView::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();
        if (r.isEmpty())
            return;

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r, 3.0f);

        if (hasSample)
        {
            const float midY = r.getCentreY();
            const float ampY = r.getHeight() * 0.46f;

            juce::Path wave;
            for (int col = 0; col < kNumPeaks; ++col)
            {
                const float x0 = r.getX() + r.getWidth() * (float) col       / (float) kNumPeaks;
                const float x1 = r.getX() + r.getWidth() * (float) (col + 1) / (float) kNumPeaks;
                const float yTop = midY - peaksMax[static_cast<size_t> (col)] * ampY;
                const float yBot = midY - peaksMin[static_cast<size_t> (col)] * ampY;
                wave.addRectangle (x0, yTop, juce::jmax (0.6f, x1 - x0 - 0.4f),
                                   juce::jmax (1.0f, yBot - yTop));
            }

            g.setColour (palette::red.withAlpha (0.22f));
            g.fillPath (wave);
            g.setColour (palette::red.withAlpha (0.85f));
            g.strokePath (wave, juce::PathStrokeType (0.6f));

            // Shade the trimmed-out edges (outside [sampleStart, sampleEnd]).
            auto* s = apvts.getRawParameterValue (id::sampleStart);
            auto* e = apvts.getRawParameterValue (id::sampleEnd);
            const float s01 = s != nullptr ? juce::jlimit (0.0f, 1.0f, s->load()) : 0.0f;
            const float e01 = juce::jmax (s01 + 0.001f,
                                          e != nullptr ? juce::jlimit (0.0f, 1.0f, e->load()) : 1.0f);

            g.setColour (juce::Colours::black.withAlpha (0.55f));
            if (s01 > 0.001f)
                g.fillRect (r.withWidth (r.getWidth() * s01));
            if (e01 < 0.999f)
                g.fillRect (r.withLeft (r.getX() + r.getWidth() * e01));
        }

        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
    }
}
