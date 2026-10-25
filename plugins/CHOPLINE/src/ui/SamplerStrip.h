// CHOPLINE — SamplerStrip (spec §4, §9; locked v17 mockup, strip_a)
// The MAIN page's top card: waveform display with slice interaction, Load
// button + drop zone, slice-mode dropdown, SENS knob, and the '…' slicing
// flyout (min slice length, zero-cross snap, sample BPM). Lean controls per
// the locked v17 design — SENS doubles as Grid division / Equal slice count.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/Engine.h"
#include "CompactKnob.h"
#include "WaveformDisplay.h"

namespace chopline::ui
{

class SamplerStrip final : public juce::Component,
                           private juce::ChangeListener,
                           public juce::FileDragAndDropTarget
{
public:
    explicit SamplerStrip (engine::Engine& engineToUse);
    ~SamplerStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /// ~60 fps tick, forwarded from the editor's VBlankAttachment.
    void animationTick() { waveform.animationTick(); }

    /// Chop selection shared with the grid strip (Phase 2).
    WaveformDisplay& getWaveform() noexcept { return waveform; }

    // Drag-and-drop loading (Finder / Explorer / DAW file drags).
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void syncControlsFromEngine();
    void openLoadChooser();
    void openSliceFlyout();

    engine::Engine& engine;

    WaveformDisplay waveform;
    juce::TextButton loadButton { "Load" };
    juce::TextButton moreButton { juce::CharPointer_UTF8 ("\xe2\x80\xa6") }; // …
    juce::ComboBox modeBox;
    CompactKnob sensKnob { "SENS" };

    std::unique_ptr<juce::FileChooser> chooser;
    bool dropHighlight = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerStrip)
};

} // namespace chopline::ui
