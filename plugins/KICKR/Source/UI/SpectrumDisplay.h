#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "DSP/Analyzer.h"

namespace kickr
{
    /**
        Stage 3 Phase 3.2 — real-time FFT spectrum.

        A 30 Hz `Timer` (only while visible) calls `Analyzer::updateSpectrum()` — the
        FFT + windowing + magnitude->dB all run on THIS message thread — then reads the
        smoothed dB frame and repaints. Draws a filled magnitude curve on a
        log-frequency x-axis (~20 Hz .. Nyquist), dB y-axis (~ -90 .. 0), the same
        red -> magenta -> violet -> lowfam accent gradient and dark radial background as
        the waveform scope, plus faint 100 / 1k / 10k Hz grid lines.
    */
    class SpectrumDisplay final : public juce::Component,
                                  private juce::Timer
    {
    public:
        explicit SpectrumDisplay (Analyzer& analyzerToUse);
        ~SpectrumDisplay() override;

        void paint (juce::Graphics&) override;
        void visibilityChanged() override;

    private:
        void timerCallback() override;

        Analyzer&  analyzer;
        juce::Path curvePath, fillPath;

        static constexpr float kMinDb =  -90.0f;
        static constexpr float kMaxDb =    0.0f;
        static constexpr float kMinHz =   20.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDisplay)
    };
}
