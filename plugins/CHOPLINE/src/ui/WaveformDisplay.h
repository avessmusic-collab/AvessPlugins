// CHOPLINE — WaveformDisplay (spec §4, §9; locked v17 mockup)
// Strip A's sample view: light hairline-grid display, dark waveform strokes
// from the peak cache, greyscale chop bands with index badges, draggable
// slice markers, click-to-audition, shift-click multi-select, wheel zoom +
// scroll, double-click add / alt-click delete in Manual mode, blue playhead
// hairlines for live voices. Reads engine state on the message thread only
// (immutable sample snapshots + model getters); audio feedback via atomics.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/Engine.h"
#include "LookAndFeel.h"

#include <set>

namespace chopline::ui
{

class WaveformDisplay final : public juce::Component
{
public:
    explicit WaveformDisplay (engine::Engine& engineToUse);

    /// Message thread: the sample or slice model changed (strip forwards the
    /// engine's change broadcasts).
    void modelChanged();

    /// ~60 fps tick from the editor's VBlankAttachment; repaints only while
    /// something animates (voices playing, loading spinner).
    void animationTick();

    // ---- chop selection (shared with the grid, Phase 2) -----------------------
    /// Selected chop ordinals in ascending index order (multi-selection drops
    /// onto the grid in this order, spec §5).
    std::vector<int> getSelectedChops() const { return { selection.begin(), selection.end() }; }
    /// Replaces the selection (grid double-click selects the chop, spec §5).
    void selectChop (int chopIndex);

    /// Drag description prefix for chop drags onto the grid.
    static constexpr const char* chopDragPrefix = "chopline:chops:";

    void paint (juce::Graphics&) override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct ChopSpan { double startSec = 0, endSec = 0; };

    std::shared_ptr<const engine::SampleData> sample() const { return engine.currentSample(); }
    std::vector<ChopSpan> chopSpans() const; // from the model's markers
    double sampleLengthSeconds() const;

    float timeToX (double sec) const;
    double xToTime (float x) const;
    int markerIdAt (float x) const;      // ±4 px hit zone; -1 = none
    int chopIndexAt (float x) const;     // -1 = none

    void clampView();
    void drawWaveform (juce::Graphics&, juce::Rectangle<float> area);
    void drawChops (juce::Graphics&, juce::Rectangle<float> area);
    void drawPlayheads (juce::Graphics&, juce::Rectangle<float> area);
    void drawEmptyOrLoading (juce::Graphics&, juce::Rectangle<float> area);

    engine::Engine& engine;

    double viewStart = 0.0; // seconds
    double viewLen = 0.0;   // seconds; 0 = full view / nothing loaded

    int hoverMarkerId = -1;
    int hoverChop = -1;
    int dragMarkerId = -1;
    int downChop = -1;           // chop under the mouse at mouseDown
    bool draggingToGrid = false; // external chop drag started
    bool draggedSinceDown = false;
    std::set<int> selection; // chop ordinals

    // animation bookkeeping
    bool wasAnimating = false;
    std::uint32_t lastVoiceSignature = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};

} // namespace chopline::ui
