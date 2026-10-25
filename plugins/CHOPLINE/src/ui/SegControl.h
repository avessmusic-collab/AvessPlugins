// CHOPLINE — SegControl (v17 "seg" widget)
// Small segmented pill: hairline cell background, active segment filled ink
// with white text. Supports disabled options (greyed, tooltip explains why) —
// used by the WARP seg where Granular/Freeze are surfaced but land in Phase 5.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "LookAndFeel.h"

namespace chopline::ui
{

class SegControl final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    juce::StringArray options;
    std::function<int()> getValue;
    std::function<void (int)> setValue;
    std::function<bool (int)> isOptionEnabled; // null = all enabled
    juce::String disabledTooltip;

    void paint (juce::Graphics& g) override
    {
        using LnF = ChoplineLookAndFeel;
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (LnF::cell);
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (LnF::hairline());
        g.drawRoundedRectangle (b, 7.0f, 1.0f);

        const auto n = juce::jmax (1, options.size());
        const auto active = getValue != nullptr ? getValue() : 0;
        const auto segW = b.getWidth() / static_cast<float> (n);

        for (int i = 0; i < options.size(); ++i)
        {
            auto seg = juce::Rectangle<float> (b.getX() + segW * static_cast<float> (i),
                                               b.getY(), segW, b.getHeight()).reduced (2.0f);
            const bool enabled = isOptionEnabled == nullptr || isOptionEnabled (i);
            if (i == active)
            {
                g.setColour (enabled ? LnF::ink : LnF::textTertiary);
                g.fillRoundedRectangle (seg, 5.0f);
            }
            g.setFont (LnF::labelFont (8.0f));
            g.setColour (i == active ? juce::Colours::white
                                     : (enabled ? LnF::label : LnF::textTertiary));
            g.drawText (options[i].toUpperCase(), seg.toNearestInt(),
                        juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (options.isEmpty() || getWidth() <= 0 || setValue == nullptr)
            return;
        const auto idx = juce::jlimit (0, options.size() - 1,
                                       static_cast<int> (e.position.x
                                           / (static_cast<float> (getWidth())
                                              / static_cast<float> (options.size()))));
        if (isOptionEnabled != nullptr && ! isOptionEnabled (idx))
            return; // disabled option (tooltip explains)
        setValue (idx);
        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        // Tooltip only while hovering a DISABLED option.
        if (isOptionEnabled == nullptr || options.isEmpty() || getWidth() <= 0)
            return;
        const auto idx = juce::jlimit (0, options.size() - 1,
                                       static_cast<int> (e.position.x
                                           / (static_cast<float> (getWidth())
                                              / static_cast<float> (options.size()))));
        setTooltip (isOptionEnabled (idx) ? juce::String() : disabledTooltip);
    }
};

} // namespace chopline::ui
