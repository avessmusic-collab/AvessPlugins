#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace kickr
{
    /**
        2026-09-02 (user request): "make the KICKR logo bigger, and when you press on it open
        a second page with licences etc, the text will be added later on."

        A full-editor overlay (added as a hidden child of the editor, brought to front on
        `show()`): dimmed backdrop + a centred panel with the wordmark, version, and three
        placeholder sections — LICENSE / CREDITS / THIRD-PARTY NOTICES — whose body text is
        `setSectionText()`-able so the real copy can be dropped in later without touching
        the layout. Closes on the CLOSE button, a click on the backdrop, or Escape.
    */
    class AboutPage : public juce::Component
    {
    public:
        AboutPage();

        void show();
        void hide();

        /** Layout scale (the editor's getWidth() / 1600). */
        void setScale (float newScale);

        enum Section { license = 0, credits, thirdParty, numSections };
        void setSectionText (Section s, const juce::String& text);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        bool keyPressed (const juce::KeyPress&) override;

        void setVersionText (const juce::String& v) { versionText = v; repaint(); }

    private:
        juce::Rectangle<int> panelBounds() const;

        float scale { 0.7f };
        juce::String versionText { "v1.0" };
        juce::TextButton closeButton { "CLOSE" };
        juce::String sectionTitles[numSections] { "LICENSE", "CREDITS", "THIRD-PARTY NOTICES" };
        juce::String sectionBodies[numSections] {
            "License text will be added here.",
            "Credits will be added here.",
            "Third-party notices will be added here."
        };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutPage)
    };
}
