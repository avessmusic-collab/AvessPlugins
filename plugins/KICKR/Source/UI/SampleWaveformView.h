#pragma once

#include <array>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Sampling/SampleLibrary.h"

namespace kickr
{
    /**
        Stage 3 Phase 3.4 — a small static waveform preview for the currently loaded
        sample, in the drag-and-drop SAMPLE strip (user request 2026-08-31).

        NOT the live oscilloscope (that's `WaveformDisplay`, which shows the post-limiter
        render of a triggered kick). This shows the RAW FILE the sample layer is reading
        from, decoded once on the message thread via `SampleLibrary::load()` (the same
        decode path the processor itself uses) whenever the selection changes, downsampled
        to a fixed number of min/max peak columns and cached for painting. The audio
        thread and its live `SampleBuffer*` are never touched here.

        A light 15 Hz `Timer` (only while visible) re-reads `sampleStart`/`sampleEnd` so
        the trim-window shading tracks those knobs live, without needing a listener.
    */
    class SampleWaveformView final : public juce::Component,
                                     private juce::Timer
    {
    public:
        explicit SampleWaveformView (juce::AudioProcessorValueTreeState& apvtsToUse);
        ~SampleWaveformView() override;

        void paint (juce::Graphics&) override;
        void visibilityChanged() override;

        /** (Re)decode `name` from `lib` if it differs from what's already shown.
            Empty name (or a load failure — e.g. a missing file) clears the view. */
        void setSample (SampleLibrary& lib, const juce::String& name);

    private:
        void timerCallback() override;

        juce::AudioProcessorValueTreeState& apvts;

        static constexpr int kNumPeaks = 160;
        std::array<float, kNumPeaks> peaksMin {};
        std::array<float, kNumPeaks> peaksMax {};
        bool          hasSample  { false };
        juce::String  cachedName;
        float         lastStart01 { -1.0f };
        float         lastEnd01   { -1.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleWaveformView)
    };
}
