#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/KickrKnob.h"
#include "DSP/Analyzer.h"

namespace kickr
{
    /**
        2026-09-02 (user request) — the FILTER page of the scope panel (third mode after
        WAVE / SPECTRUM): an Ableton-Auto-Filter-style view of the master filter.

          - response graph on a log-frequency axis (20 Hz .. 20 kHz, -36..+24 dB) with the
            12 dB LP / HP curve of the current settings and a draggable node: X = cutoff,
            Y = resonance (up = more), like Ableton's filter display;
          - LP / HP type buttons (radio), an ON pill (`filterOn`), FREQ / RES knobs.

        2026-09-02 (follow-up, user: "make so that the spectrum shows behind the filter when
        filter is on, dont change spectrum page, only add the visual to the filter"): while
        the filter is ON the live FFT spectrum (the same `Analyzer` frame the SPECTRUM page
        reads, pulled by this page's own 30 Hz timer) is drawn dimly BEHIND the response
        curve on the same log-frequency axis, so you see what the curve is cutting. Off ->
        no spectrum, timer stopped. The SPECTRUM page itself is untouched.

        Bindings go straight to the APVTS: the knobs / pill through KickrKnob / KickrToggle
        attachments, the type buttons and the node drag through `juce::ParameterAttachment`
        (begin / setValueAsPartOfGesture / end), so host automation and the graph stay in
        sync in both directions.
    */
    class FilterDisplay : public juce::Component,
                          private juce::Timer
    {
    public:
        FilterDisplay (juce::AudioProcessorValueTreeState& apvts, Analyzer& analyzer);
        ~FilterDisplay() override;

        void visibilityChanged() override;
        /** Pull a spectrum frame + repaint now (headless snapshot tests). */
        void refreshNow() { timerCallback(); }

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp   (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        void updateTimer();
        bool filterIsOn() const noexcept;

        juce::Rectangle<float> graphBounds() const;
        float xForFreq (float hz, juce::Rectangle<float> g) const;
        float freqForX (float x,  juce::Rectangle<float> g) const;
        void  setTypeFromButtons();
        void  syncTypeButtons();

        juce::AudioProcessorValueTreeState& apvts;
        Analyzer& analyzer;
        juce::Path spectrumFill;   // rebuilt each frame while the filter is on
        std::vector<float> spectrumCols;
        juce::RangedAudioParameter* pType { nullptr };
        juce::RangedAudioParameter* pFreq { nullptr };
        juce::RangedAudioParameter* pRes  { nullptr };
        juce::RangedAudioParameter* pOn   { nullptr };

        juce::TextButton lpButton { "LOW PASS" };
        juce::TextButton hpButton { "HIGH PASS" };
        std::unique_ptr<KickrToggle> onToggle;
        std::unique_ptr<KickrKnob>   freqKnob, resKnob;

        std::unique_ptr<juce::ParameterAttachment> typeAtt, freqAtt, resAtt, onAtt;
        bool dragging { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterDisplay)
    };
}
