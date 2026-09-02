#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "DSP/Analyzer.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

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
        /*  2026-09-02 (user request): Pro-Q-3-style analyzer — one energy-averaged value per
            pixel column on the log axis (see SpectrumCurve.h), instant attack / Speed-set
            release, and the four Pro-Q settings as small selectors on the page:
            RANGE 60/90/120 dB, RES Low/Medium/High/Max (FFT 2048..16384, overlapping
            frames), SPEED Very Slow..Very Fast, TILT 0..6 dB/oct. Persisted as state
            properties on the APVTS tree (view settings, not parameters). */
        SpectrumDisplay (Analyzer& analyzerToUse, juce::AudioProcessorValueTreeState& apvts);
        void resized() override;
        ~SpectrumDisplay() override;

        void paint (juce::Graphics&) override;
        void visibilityChanged() override;

        /** Pull a frame + repaint immediately (headless snapshot tests). */
        void refreshNow() { timerCallback(); }

    private:
        void timerCallback() override;

        void applySettings (bool store);
        void loadSettings();

        Analyzer&  analyzer;
        juce::AudioProcessorValueTreeState& apvts;
        juce::ComboBox rangeBox, resBox, speedBox, tiltBox;
        std::vector<float> columns;
        juce::Path curvePath, fillPath;

        static constexpr float kMinDb =  -90.0f;
        static constexpr float kMaxDb =    0.0f;
        static constexpr float kMinHz =   20.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDisplay)
    };
}
