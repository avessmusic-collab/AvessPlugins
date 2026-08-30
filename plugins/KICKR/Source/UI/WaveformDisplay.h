#pragma once

#include <array>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DSP/Analyzer.h"

namespace kickr
{
    /**
        Stage 3 Phase 3.2 — real-time kick-waveform scope.

        Pulls one full post-limiter capture per trigger from `kickr::Analyzer`
        (double-buffered + atomic index — zero audio-thread cost) on a 30 Hz `Timer`
        that only runs while the component is visible. Paints the mockup's scope: dark
        radial-gradient panel, faint centre line + grid, a soft under-fill, a fat
        low-alpha glow stroke then a crisp thin stroke in a horizontal
        red -> magenta -> violet -> lowfam gradient, a millisecond axis, and a KICKR
        watermark bottom-right.
    */
    class WaveformDisplay final : public juce::Component,
                                  private juce::Timer
    {
    public:
        explicit WaveformDisplay (Analyzer& analyzerToUse);
        ~WaveformDisplay() override;

        void paint (juce::Graphics&) override;
        void visibilityChanged() override;

    private:
        void timerCallback() override;

        Analyzer& analyzer;

        std::array<float, Analyzer::kWaveCaptureLen> wave {};
        juce::Path tracePath, fillPath;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
    };
}
