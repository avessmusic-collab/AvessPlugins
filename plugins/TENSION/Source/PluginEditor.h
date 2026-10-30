#pragma once

#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/ControlWidgets.h"
#include "UI/CustomLookAndFeel.h"
#include "UI/TensionKnob.h"
#include "UI/Visualizer.h"

// Native JUCE editor (NO WebView). Fixed 520x520 per the locked v4 mockup.
// GUI Phase 4.1: layout + static rendering. Controls are interactive but NOT yet
// bound to the APVTS (Phase 4.2); the visualizer is fed from the TENSION knob's
// own value (Phase 4.3 switches it to processor telemetry).
class TENSIONAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TENSIONAudioProcessorEditor (TENSIONAudioProcessor&);
    ~TENSIONAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Dev/preview hook (used by TENSION_UISnapshot): set tension (0..100) and rise mode.
    void setPreviewState (double tensionPercent, int riseMode);

private:
    void timerCallback() override;
    void pushVisualizerState();
    void applyRiseMode (int mode, bool animate);

    TENSIONAudioProcessor& processorRef;

    // Declaration order: the LookAndFeel (and its ThemeState) outlive every
    // child that references them.
    CustomLookAndFeel lnf;

    Visualizer visualizer;
    TensionKnob tensionKnob;
    TensionKnob mixKnob;
    TensionKnob outputKnob;
    RiseModeSelector riseSelector;
    SyncPill syncPill;
    EnclosureBezel bezel;

    double lastTimerMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TENSIONAudioProcessorEditor)
};
