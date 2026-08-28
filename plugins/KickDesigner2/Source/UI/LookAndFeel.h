#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace kd2
{
    /**
        Stage 1 stub. Implemented in Stage 3 (plan.md Phase 3.1 / 3.4).

        Premium dark modern LookAndFeel shared by the custom Knob / WaveformDisplay /
        SpectrumDisplay / EnvelopeDisplay / PresetBrowser components. Deep neutral
        background, restrained accent colour, crisp typography, subtle depth — no
        skeuomorphism.
    */
    class KickDesigner2LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        KickDesigner2LookAndFeel() = default;
    };
}
