#pragma once

#include <array>
#include <cstdint>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DSP/Analyzer.h"

namespace kickr
{
    /**
        Stage 3 Phase 3.2 — real-time kick-waveform scope.

        Streams the post-limiter capture from `kickr::Analyzer` on a 60 Hz `Timer`
        (only while visible — zero audio-thread cost either way). The trace is drawn
        against a FIXED ~341 ms time window, only up to the samples captured so far, so
        the kick **sweeps in left -> right like a pulse** with ~one timer tick of
        latency. Mockup styling: dark radial-gradient panel, faint centre line + grid,
        a soft under-fill, a fat low-alpha glow stroke then a crisp thin stroke in a
        horizontal red -> magenta -> violet -> lowfam gradient, a millisecond axis, and
        a KICKR watermark bottom-right.

        2026-09-01: capture sample 0 IS the note-on's own sample-accurate offset (see
        `Analyzer::armCapture`'s `skipSamples` — the mid-block leading silence is dropped
        at the SOURCE now, on the audio thread), so the trace always starts at x=0 with
        no empty space, at a guaranteed-fixed scale, for every trigger no matter its
        timing. An earlier version tried to detect this here instead, via an amplitude
        threshold on the raw (block-start-aligned) capture — that couldn't distinguish a
        real new onset from a still-ringing previous voice's tail bleeding into a fast
        retrigger's capture, so the trim amount (and therefore the trace) was NOT
        actually stable (user report: "no matter what time i input midi at, the
        soundwave ... appears always fixed and doesn't move around" — it didn't, yet).
        Fixing it at the source removes the ambiguity entirely.
    */
    class WaveformDisplay final : public juce::Component,
                                  private juce::Timer
    {
    public:
        explicit WaveformDisplay (Analyzer& analyzerToUse);
        ~WaveformDisplay() override;

        void paint (juce::Graphics&) override;
        void visibilityChanged() override;

        /** Pull a frame + repaint immediately (used for headless snapshot tests where
            the Timer never fires). */
        void refreshNow() { timerCallback(); }

    private:
        void timerCallback() override;

        Analyzer& analyzer;

        std::array<float, Analyzer::kWaveCaptureLen> wave {};
        int           validLen  { 0 };          // samples captured so far this trigger
        std::uint32_t generation { 0 };         // last-seen trigger tag
        juce::Path tracePath, fillPath;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
    };
}
