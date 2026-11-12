#include "UI/FilterDisplay.h"
#include "UI/KickrLookAndFeel.h"
#include "Parameters/ParameterIDs.h"
#include "Parameters/ParameterDescriptions.h"
#include "DSP/MasterFilter.h"
#include "UI/SpectrumCurve.h"

#include <cmath>

namespace kickr
{
    namespace pal = palette;

    FilterDisplay::FilterDisplay (juce::AudioProcessorValueTreeState& state, Analyzer& analyzerToUse)
        : apvts (state), analyzer (analyzerToUse)
    {
        pType  = apvts.getParameter (id::filterType);
        pFreq  = apvts.getParameter (id::filterFreq);
        pRes   = apvts.getParameter (id::filterRes);
        pOn    = apvts.getParameter (id::filterOn);
        pSlope = apvts.getParameter (id::filterSlope);   // 2026-09-07
        jassert (pType != nullptr && pFreq != nullptr && pRes != nullptr && pOn != nullptr && pSlope != nullptr);

        for (auto* b : { &lpButton, &hpButton })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (0x71f7);
            b->onClick = [this] { setTypeFromButtons(); };
            addAndMakeVisible (b);
        }
        lpButton.setTooltip ("Low pass - keeps what's below the cutoff");
        hpButton.setTooltip ("High pass - keeps what's above the cutoff");

        // 2026-09-07 — 12/18/24 dB/oct slope radio row.
        for (auto* b : { &slope12Button, &slope18Button, &slope24Button })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (0x71f8);
            b->onClick = [this] { setSlopeFromButtons(); };
            b->setTooltip (paramDescription (id::filterSlope));
            addAndMakeVisible (b);
        }

        onToggle = std::make_unique<KickrToggle> (apvts, id::filterOn, "ON", pal::red);
        addAndMakeVisible (*onToggle);
        freqKnob = std::make_unique<KickrKnob> (apvts, id::filterFreq, "FREQ", pal::lowfam, false, KickrKnob::Size::Small);
        resKnob  = std::make_unique<KickrKnob> (apvts, id::filterRes,  "RES",  pal::magenta, false, KickrKnob::Size::Small);
        addAndMakeVisible (*freqKnob);
        addAndMakeVisible (*resKnob);

        auto repaintCb = [this] (float) { repaint(); };
        if (pType  != nullptr) typeAtt  = std::make_unique<juce::ParameterAttachment> (*pType, [this] (float) { syncTypeButtons(); repaint(); }, apvts.undoManager);
        if (pFreq  != nullptr) freqAtt  = std::make_unique<juce::ParameterAttachment> (*pFreq, repaintCb, apvts.undoManager);
        if (pRes   != nullptr) resAtt   = std::make_unique<juce::ParameterAttachment> (*pRes,  repaintCb, apvts.undoManager);
        if (pOn    != nullptr) onAtt    = std::make_unique<juce::ParameterAttachment> (*pOn,   [this] (float) { updateTimer(); repaint(); }, apvts.undoManager);
        if (pSlope != nullptr) slopeAtt = std::make_unique<juce::ParameterAttachment> (*pSlope, [this] (float) { syncSlopeButtons(); repaint(); }, apvts.undoManager);
        for (auto* a : { typeAtt.get(), freqAtt.get(), resAtt.get(), onAtt.get(), slopeAtt.get() })
            if (a != nullptr) a->sendInitialUpdate();
    }

    FilterDisplay::~FilterDisplay()
    {
        stopTimer();
    }

    bool FilterDisplay::filterIsOn() const noexcept
    {
        return pOn != nullptr && pOn->getValue() > 0.5f;
    }

    void FilterDisplay::updateTimer()
    {
        // Live spectrum only while this page is showing AND the filter is on.
        if (isVisible() && filterIsOn())
        {
            if (! isTimerRunning())
                startTimerHz (30);
        }
        else
        {
            stopTimer();
            spectrumFill.clear();
        }
    }

    void FilterDisplay::visibilityChanged()
    {
        updateTimer();
        if (isVisible())
            repaint();
    }

    void FilterDisplay::timerCallback()
    {
        analyzer.updateSpectrum();   // FFT / windowing / dB — message thread only (same call the SPECTRUM page makes)
        repaint();
    }

    void FilterDisplay::setTypeFromButtons()
    {
        if (pType == nullptr || typeAtt == nullptr) return;
        const float idx = hpButton.getToggleState() ? 1.0f : 0.0f;
        typeAtt->setValueAsCompleteGesture (idx);
    }

    void FilterDisplay::syncTypeButtons()
    {
        if (pType == nullptr) return;
        const bool hp = pType->convertFrom0to1 (pType->getValue()) > 0.5f;
        lpButton.setToggleState (! hp, juce::dontSendNotification);
        hpButton.setToggleState (hp,   juce::dontSendNotification);
    }

    void FilterDisplay::setSlopeFromButtons()
    {
        if (pSlope == nullptr || slopeAtt == nullptr) return;
        const float idx = slope24Button.getToggleState() ? 2.0f
                        : slope18Button.getToggleState() ? 1.0f : 0.0f;
        slopeAtt->setValueAsCompleteGesture (idx);
    }

    void FilterDisplay::syncSlopeButtons()
    {
        if (pSlope == nullptr) return;
        const int idx = juce::jlimit (0, 2, (int) std::lround (pSlope->convertFrom0to1 (pSlope->getValue())));
        slope12Button.setToggleState (idx == 0, juce::dontSendNotification);
        slope18Button.setToggleState (idx == 1, juce::dontSendNotification);
        slope24Button.setToggleState (idx == 2, juce::dontSendNotification);
    }

    juce::Rectangle<float> FilterDisplay::graphBounds() const
    {
        const float s = juce::jmax (0.5f, (float) getWidth() / 1120.0f);
        auto r = getLocalBounds().toFloat().reduced (14.0f * s, 12.0f * s);
        r.removeFromTop (30.0f * s);      // mode buttons row (owned by the editor) + type buttons
        r.removeFromLeft (118.0f * s);    // left column: LP / HP / ON
        r.removeFromRight (150.0f * s);   // right column: FREQ / RES knobs
        return r;
    }

    float FilterDisplay::xForFreq (float hz, juce::Rectangle<float> g) const
    {
        const float t = std::log (juce::jlimit (20.0f, 20000.0f, hz) / 20.0f) / std::log (1000.0f);
        return g.getX() + t * g.getWidth();
    }

    float FilterDisplay::freqForX (float x, juce::Rectangle<float> g) const
    {
        const float t = juce::jlimit (0.0f, 1.0f, (x - g.getX()) / juce::jmax (1.0f, g.getWidth()));
        return 20.0f * std::pow (1000.0f, t);
    }

    void FilterDisplay::resized()
    {
        const float s = juce::jmax (0.5f, (float) getWidth() / 1120.0f);
        const auto sc = [s] (float v) { return juce::roundToInt (v * s); };

        auto r = getLocalBounds().reduced (sc (14), sc (12));
        r.removeFromTop (sc (30));   // keep clear of the WAVE / SPECTRUM / FILTER buttons (top-right)

        auto left = r.removeFromLeft (sc (108));
        r.removeFromLeft (sc (10));
        lpButton.setBounds (left.removeFromTop (sc (22)));
        left.removeFromTop (sc (6));
        hpButton.setBounds (left.removeFromTop (sc (22)));
        left.removeFromTop (sc (6));
        // 2026-09-07 — 12 / 18 / 24 slope radio row under the type buttons.
        {
            auto slopeRow = left.removeFromTop (sc (22));
            const int bw = slopeRow.getWidth() / 3;
            slope12Button.setBounds (slopeRow.removeFromLeft (bw).reduced (sc (1), 0));
            slope18Button.setBounds (slopeRow.removeFromLeft (bw).reduced (sc (1), 0));
            slope24Button.setBounds (slopeRow.reduced (sc (1), 0));
        }
        left.removeFromTop (sc (10));
        onToggle->setBounds (left.removeFromTop (sc (22)).withSizeKeepingCentre (sc (64), sc (22)));

        auto right = r.removeFromRight (sc (140));
        r.removeFromRight (sc (10));
        const int kw = right.getWidth() / 2;
        auto knobs = right.withSizeKeepingCentre (right.getWidth(), juce::jmin (right.getHeight(), sc (84)));
        freqKnob->setBounds (knobs.removeFromLeft (kw).reduced (sc (2)));
        resKnob->setBounds  (knobs.reduced (sc (2)));
    }

    void FilterDisplay::paint (juce::Graphics& g)
    {
        const auto rf = getLocalBounds().toFloat();
        if (rf.isEmpty()) return;

        const float s      = juce::jmax (0.5f, rf.getWidth() / 1120.0f);
        const float radius = juce::jmin (18.0f, rf.getHeight() * 0.10f);

        // Same recessed dark panel as the WAVE / SPECTRUM pages.
        {
            juce::Path clip; clip.addRoundedRectangle (rf, radius); g.reduceClipRegion (clip);
        }
        juce::ColourGradient bg (juce::Colour (0xff17111F), rf.getCentreX(), rf.getY() - rf.getHeight() * 0.2f,
                                 juce::Colour (0xff060409), rf.getCentreX(), rf.getBottom(), true);
        bg.addColour (0.52, juce::Colour (0xff0A0710));
        g.setGradientFill (bg);
        g.fillRoundedRectangle (rf, radius);
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (rf.reduced (0.5f), radius, 1.4f);

        const auto gr = graphBounds();
        if (gr.getWidth() < 20.0f || gr.getHeight() < 20.0f) return;

        const bool  on    = pOn   != nullptr && pOn->getValue() > 0.5f;
        const bool  hp    = pType != nullptr && pType->convertFrom0to1 (pType->getValue()) > 0.5f;
        const float freq  = pFreq != nullptr ? pFreq->convertFrom0to1 (pFreq->getValue()) : 1000.0f;
        const float res   = pRes  != nullptr ? pRes->convertFrom0to1 (pRes->getValue())   : 0.2f;
        const float q     = MasterFilter::qFromResonance (res);
        const int   slope = pSlope != nullptr ? juce::jlimit (0, 2, (int) std::lround (pSlope->convertFrom0to1 (pSlope->getValue()))) : 1;

        // Grid: decades + a few in-between, 0 dB line.
        g.setColour (pal::ink.withAlpha (0.07f));
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            g.drawVerticalLine (juce::roundToInt (xForFreq (f, gr)), gr.getY(), gr.getBottom());
        const float dbMin = -36.0f, dbMax = 24.0f;
        auto yForDb = [&] (float db) { return gr.getBottom() - (juce::jlimit (dbMin, dbMax, db) - dbMin) / (dbMax - dbMin) * gr.getHeight(); };
        g.setColour (pal::ink.withAlpha (0.11f));
        g.drawHorizontalLine (juce::roundToInt (yForDb (0.0f)), gr.getX(), gr.getRight());
        g.setColour (pal::ink.withAlpha (0.45f));
        g.setFont (KickrLookAndFeel::microFont (8.0f * s));
        for (float f : { 100.0f, 1000.0f, 10000.0f })
            g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f, 0) + " kHz" : juce::String (f, 0) + " Hz",
                        juce::Rectangle<float> (xForFreq (f, gr) - 24.0f * s, gr.getBottom() + 1.0f, 48.0f * s, 10.0f * s),
                        juce::Justification::centred, false);

        // Response curve of the actual cascade (2026-09-07 slope-aware, mirrors MasterFilter):
        //   stage 1 (always)   2nd-order SVF, user Q: |H| = 1/sqrt((1-x^2)^2 + (x/Q)^2), HP *= x^2
        //   slope 18 dB adds   1st-order pole:        |H| *= 1/sqrt(1+x^2), HP *= x
        //   slope 24 dB adds   2nd-order @ Q=0.707
        juce::Path curve;
        const int n = juce::jmax (32, (int) gr.getWidth());
        for (int i = 0; i <= n; ++i)
        {
            const float x  = gr.getX() + gr.getWidth() * (float) i / (float) n;
            const float f  = freqForX (x, gr);
            const float xr = f / freq;
            const float x2 = xr * xr;
            float mag = 1.0f / std::sqrt (std::max (1.0e-12f, (1.0f - x2) * (1.0f - x2) + x2 / (q * q)));
            if (hp) mag *= x2;
            if (slope == 1)
            {
                float m1 = 1.0f / std::sqrt (1.0f + x2);
                if (hp) m1 *= xr;
                mag *= m1;
            }
            else if (slope == 2)
            {
                float m2 = 1.0f / std::sqrt (std::max (1.0e-12f, (1.0f - x2) * (1.0f - x2) + x2 * 2.0f));   // Q = 0.707 -> (x/Q)^2 = 2 x^2
                if (hp) m2 *= x2;
                mag *= m2;
            }
            const float db = 20.0f * std::log10 (std::max (1.0e-6f, mag));
            const float y  = yForDb (db);
            if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
        }
        // Live spectrum behind the curve while the filter is on (2026-09-02 follow-up).
        if (on)
        {
            // Same Pro-Q-style curve as the SPECTRUM page (energy-averaged per column, tilted,
            // Range from the analyzer view) on this page's 20 Hz - 20 kHz axis.
            const float sMin = -analyzer.getView().rangeDb, sMax = 0.0f;
            auto ySpec = [&] (float db) { return gr.getBottom() - (juce::jlimit (sMin, sMax, db) - sMin) / (sMax - sMin) * gr.getHeight(); };
            const int numCols = juce::jmax (2, (int) gr.getWidth());
            buildSpectrumColumns (analyzer, numCols, 20.0f, 20000.0f, spectrumCols);

            spectrumFill.clear();
            bool started = false;
            for (int i = 0; i < numCols; ++i)
            {
                const float x = gr.getX() + (float) i;
                const float y = ySpec (spectrumCols[static_cast<size_t> (i)]);
                if (! started) { spectrumFill.startNewSubPath (x, gr.getBottom()); spectrumFill.lineTo (x, y); started = true; }
                else            spectrumFill.lineTo (x, y);
            }
            if (started)
            {
                spectrumFill.lineTo (gr.getRight(), gr.getBottom());
                spectrumFill.closeSubPath();
                juce::ColourGradient sg (pal::red.withAlpha (0.40f), gr.getX(), gr.getY(),
                                         pal::violet.withAlpha (0.16f), gr.getX(), gr.getBottom(), false);
                sg.addColour (0.5, pal::magenta.withAlpha (0.28f));
                g.setGradientFill (sg);
                g.fillPath (spectrumFill);
                g.setColour (pal::magenta.withAlpha (0.55f));
                g.strokePath (spectrumFill, juce::PathStrokeType (1.0f * s));
            }
        }

        const auto accent = on ? pal::lowfam : pal::inkDim;
        {
            juce::Path fill (curve);
            fill.lineTo (gr.getRight(), gr.getBottom());
            fill.lineTo (gr.getX(), gr.getBottom());
            fill.closeSubPath();
            g.setColour (accent.withAlpha (on ? 0.14f : 0.06f));
            g.fillPath (fill);
        }
        g.setColour (accent.withAlpha (on ? 0.95f : 0.5f));
        g.strokePath (curve, juce::PathStrokeType (2.0f * s, juce::PathStrokeType::curved));

        // Node: X = cutoff, Y = resonance (up = more).
        const float nx = xForFreq (freq, gr);
        const float ny = gr.getBottom() - res * gr.getHeight();
        g.setColour (accent.withAlpha (0.25f));
        g.fillEllipse (nx - 9.0f * s, ny - 9.0f * s, 18.0f * s, 18.0f * s);
        g.setColour (on ? pal::inkHi : pal::ink);
        g.fillEllipse (nx - 4.5f * s, ny - 4.5f * s, 9.0f * s, 9.0f * s);

        // Readout.
        g.setColour (pal::ink.withAlpha (0.8f));
        g.setFont (KickrLookAndFeel::microFont (9.0f * s));
        const juce::String txt = (hp ? "HP " : "LP ") + (freq >= 1000.0f ? juce::String (freq / 1000.0f, 2) + " kHz" : juce::String (freq, 0) + " Hz")
                               + "   " + juce::String (12 + slope * 6) + " dB/oct"
                               + "   res " + juce::String (res, 2) + (on ? "" : "   (off)");
        g.drawText (txt, gr.withHeight (14.0f * s).translated (0.0f, 2.0f * s), juce::Justification::topLeft, false);
    }

    void FilterDisplay::mouseDown (const juce::MouseEvent& e)
    {
        if (! graphBounds().contains (e.position)) return;
        dragging = true;
        if (freqAtt) freqAtt->beginGesture();
        if (resAtt)  resAtt->beginGesture();
        mouseDrag (e);
    }

    void FilterDisplay::mouseDrag (const juce::MouseEvent& e)
    {
        if (! dragging) return;
        const auto gr = graphBounds();
        const float f = freqForX (e.position.x, gr);
        const float r = juce::jlimit (0.0f, 1.0f, (gr.getBottom() - e.position.y) / juce::jmax (1.0f, gr.getHeight()));
        if (freqAtt && pFreq) freqAtt->setValueAsPartOfGesture (f);
        if (resAtt  && pRes)  resAtt->setValueAsPartOfGesture (r);
    }

    void FilterDisplay::mouseUp (const juce::MouseEvent&)
    {
        if (! dragging) return;
        dragging = false;
        if (freqAtt) freqAtt->endGesture();
        if (resAtt)  resAtt->endGesture();
    }
}
