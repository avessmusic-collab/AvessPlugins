#include "UI/AboutPage.h"
#include "UI/KickrLookAndFeel.h"

namespace kickr
{
    AboutPage::AboutPage()
    {
        setWantsKeyboardFocus (true);
        closeButton.onClick = [this] { hide(); };
        closeButton.setTooltip ("Close");
        addAndMakeVisible (closeButton);
        setVisible (false);
    }

    void AboutPage::show()
    {
        setVisible (true);
        toFront (false);
        grabKeyboardFocus();
        repaint();
    }

    void AboutPage::hide()
    {
        setVisible (false);
    }

    void AboutPage::setScale (float newScale)
    {
        scale = juce::jmax (0.1f, newScale);
        resized();
        repaint();
    }

    void AboutPage::setSectionText (Section s, const juce::String& text)
    {
        if (s >= 0 && s < numSections)
        {
            sectionBodies[s] = text;
            repaint();
        }
    }

    juce::Rectangle<int> AboutPage::panelBounds() const
    {
        const int w = juce::roundToInt (760.0f * scale);
        const int h = juce::roundToInt (560.0f * scale);
        return getLocalBounds().withSizeKeepingCentre (juce::jmin (getWidth() - 40, w),
                                                       juce::jmin (getHeight() - 40, h));
    }

    void AboutPage::resized()
    {
        auto p = panelBounds();
        const auto sc = [this] (int v) { return juce::roundToInt ((float) v * scale); };
        closeButton.setBounds (p.removeFromBottom (sc (64)).withSizeKeepingCentre (sc (140), sc (30)));
    }

    void AboutPage::paint (juce::Graphics& g)
    {
        const auto sc = [this] (int v) { return (float) v * scale; };

        // Backdrop: dim the whole editor.
        g.fillAll (palette::well.withAlpha (0.78f));

        // Panel.
        auto p = panelBounds().toFloat();
        juce::ColourGradient pg (palette::panelHi, 0.0f, p.getY(), palette::panelLo, 0.0f, p.getBottom(), false);
        g.setGradientFill (pg);
        g.fillRoundedRectangle (p, sc (20));
        g.setColour (palette::panelEdge.withAlpha (0.8f));
        g.drawRoundedRectangle (p.reduced (0.5f), sc (20), 1.0f);

        auto inner = p.reduced (sc (36), sc (28));

        // Wordmark + version.
        auto head = inner.removeFromTop (sc (54));
        g.setColour (palette::inkHi);
        g.setFont (KickrLookAndFeel::titleFont (sc (34)));
        g.drawText ("KICKR", head.removeFromLeft (sc (220)), juce::Justification::centredLeft, false);
        g.setColour (palette::inkDim);
        g.setFont (KickrLookAndFeel::microFont (sc (12)));
        g.drawText (versionText, head, juce::Justification::centredLeft, false);

        inner.removeFromTop (sc (6));
        g.setColour (palette::panelEdge.withAlpha (0.7f));
        g.drawHorizontalLine (juce::roundToInt (inner.getY()), inner.getX(), inner.getRight());
        inner.removeFromTop (sc (18));
        inner.removeFromBottom (sc (64));   // CLOSE button row

        // Three sections, stacked.
        const float sectionH = inner.getHeight() / (float) numSections;
        for (int i = 0; i < numSections; ++i)
        {
            auto sec = inner.removeFromTop (sectionH);
            g.setColour (palette::red);
            g.setFont (KickrLookAndFeel::titleFont (sc (12)));
            g.drawText (sectionTitles[i], sec.removeFromTop (sc (20)), juce::Justification::centredLeft, false);
            sec.removeFromTop (sc (4));
            g.setColour (palette::ink);
            g.setFont (KickrLookAndFeel::microFont (sc (12)));
            g.drawFittedText (sectionBodies[i], sec.toNearestInt(), juce::Justification::topLeft, 12, 1.0f);
        }
    }

    void AboutPage::mouseDown (const juce::MouseEvent& e)
    {
        if (! panelBounds().contains (e.getPosition()))
            hide();   // click on the backdrop closes
    }

    bool AboutPage::keyPressed (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::escapeKey)
        {
            hide();
            return true;
        }
        return false;
    }
}
