#include "PluginEditor.h"

namespace
{
    juce::String percentText (double v) { return juce::String (juce::roundToInt (v)) + "%"; }

    juce::String dbText (double v)
    {
        const auto s = juce::String (v, 1);
        return (v > 0.04 ? "+" : "") + s + " dB";
    }
}

TENSIONAudioProcessorEditor::TENSIONAudioProcessorEditor (TENSIONAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p),
      visualizer (lnf.getTheme()),
      tensionKnob (lnf.getTheme(), TensionKnob::Kind::Hero, "TENSION", percentText, 0.0, 100.0, 0.0),
      mixKnob (lnf.getTheme(), TensionKnob::Kind::Compact, "MIX", percentText, 0.0, 100.0, 100.0),
      outputKnob (lnf.getTheme(), TensionKnob::Kind::Compact, "OUTPUT", dbText, -24.0, 12.0, 0.0),
      riseSelector (lnf.getTheme()),
      syncPill (lnf.getTheme())
{
    setLookAndFeel (&lnf);
    setOpaque (true);

    addAndMakeVisible (visualizer);       // background layer
    addAndMakeVisible (tensionKnob);
    addAndMakeVisible (mixKnob);
    addAndMakeVisible (outputKnob);
    addAndMakeVisible (riseSelector);
    addAndMakeVisible (syncPill);
    addAndMakeVisible (bezel);            // always on top

    // Phase 4.1: local wiring only (4.2 replaces these with APVTS attachments).
    tensionKnob.onValueChange = [this] { pushVisualizerState(); };
    riseSelector.onChange = [this] (int mode) { applyRiseMode (mode, true); };

    applyRiseMode (riseSelector.getSelectedIndex(), false);
    pushVisualizerState();

    setSize (TensionUI::kWidth, TensionUI::kHeight);   // fixed, non-resizable
    setResizable (false, false);

    lastTimerMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

TENSIONAudioProcessorEditor::~TENSIONAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void TENSIONAudioProcessorEditor::setPreviewState (double tensionPercent, int riseMode)
{
    tensionKnob.setValue (tensionPercent, juce::dontSendNotification);
    riseSelector.setSelectedIndex (riseMode, false);
    applyRiseMode (riseMode, false);
    pushVisualizerState();
}

void TENSIONAudioProcessorEditor::applyRiseMode (int mode, bool animate)
{
    lnf.getTheme().setTarget (mode, animate);
    visualizer.setRiseMode (mode);
    if (! animate)
        timerCallback();   // refresh controls immediately
}

void TENSIONAudioProcessorEditor::pushVisualizerState()
{
    Visualizer::State s;
    s.tension = (float) (tensionKnob.getValue() / 100.0);
    visualizer.setState (s);
}

// 60 Hz: advances the 450 ms theme crossfade and repaints themed controls.
void TENSIONAudioProcessorEditor::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = juce::jlimit (0.0, 100.0, now - lastTimerMs);
    lastTimerMs = now;

    if (lnf.getTheme().advance (dt))
    {
        visualizer.repaint();
        tensionKnob.repaint();
        mixKnob.repaint();
        outputKnob.repaint();
        riseSelector.repaint();
        syncPill.repaint();
    }
}

void TENSIONAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff050405));
}

void TENSIONAudioProcessorEditor::resized()
{
    // Layout from the v4 yaml (520x520 canvas). Bounds are set here, never in
    // the constructor (juce8-critical-patterns #6).
    visualizer.setBounds (getLocalBounds());
    bezel.setBounds (getLocalBounds());

    tensionKnob.setBounds (TensionKnob::boundsForCentre (TensionKnob::Kind::Hero, { 260, 235 }));
    mixKnob.setBounds (TensionKnob::boundsForCentre (TensionKnob::Kind::Compact, { 68, 448 }));
    outputKnob.setBounds (TensionKnob::boundsForCentre (TensionKnob::Kind::Compact, { 452, 448 }));
    riseSelector.setBounds (RiseModeSelector::defaultBounds());
    syncPill.setBounds (SyncPill::defaultBounds());
}
