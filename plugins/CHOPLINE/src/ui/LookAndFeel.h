// CHOPLINE — LookAndFeel (spec §9, creative brief v17)
// Wireframe-light skin: paper-white cards on light grey, near-black ink
// (#0d0d0f), label grey (#2a2a2e), blue #3a6df0 accent, red #ff5a4f strictly
// for Beat Repeat activity. Full design-token table lands in Phase 5 (visual
// polish, after the user gives direction).
//
// Phase 0: stub — base colours only.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace chopline::ui
{

class ChoplineLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    // Design tokens (v17 mockup). One-place restyling per spec §9.
    static const juce::Colour paper;   // card / window background
    static const juce::Colour chassis; // outer background grey
    static const juce::Colour ink;     // near-black strokes, notches, text
    static const juce::Colour label;   // titles / labels
    static const juce::Colour accent;  // selection / playhead blue
    static const juce::Colour repeat;  // Beat Repeat activity red (only)

    ChoplineLookAndFeel();
};

} // namespace chopline::ui
