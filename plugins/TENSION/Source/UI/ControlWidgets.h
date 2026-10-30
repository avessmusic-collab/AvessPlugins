#pragma once

#include <functional>

#include "CustomLookAndFeel.h"

// RISE MODE: 3-way segmented pill ("Slow Burn" / "Cinematic" / "Aggressive")
// with its "RISE MODE" caption above. Component bounds 220x46; the pill itself
// is the lower 30 px. Phase 4.2 binds it to the riseMode choice parameter.
class RiseModeSelector : public juce::Component
{
public:
    explicit RiseModeSelector (const TensionUI::ThemeState& themeState);

    void setSelectedIndex (int index, bool notify);
    int  getSelectedIndex() const noexcept { return selected; }

    std::function<void (int)> onChange;

    static juce::Rectangle<int> defaultBounds() { return { 115, 420, 290, 50 }; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    const TensionUI::ThemeState& theme;
    int selected = TensionUI::Cinematic;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RiseModeSelector)
};

// SYNC: 72x26 pill toggle with LED. Phase 4.2 binds it to the sync bool.
class SyncPill : public juce::Component
{
public:
    explicit SyncPill (const TensionUI::ThemeState& themeState);

    void setOn (bool on, bool notify);
    bool isOn() const noexcept { return on; }

    std::function<void (bool)> onChange;

    static juce::Rectangle<int> defaultBounds() { return { 424, 24, 72, 26 }; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    const TensionUI::ThemeState& theme;
    bool on = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncPill)
};

// Machined enclosure bezel + TENSION wordmark. Sits on top of every other
// component (mouse-transparent, buffered to an image so it renders once).
class EnclosureBezel : public juce::Component
{
public:
    EnclosureBezel();
    void paint (juce::Graphics&) override;
    bool hitTest (int, int) override { return false; }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnclosureBezel)
};
