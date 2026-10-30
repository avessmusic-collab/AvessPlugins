#include "PluginEditor.h"

namespace
{
    juce::String percentText (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; }

    juce::String dbText (double v)
    {
        const auto s = juce::String (v, 1);
        return (v > 0.04 ? "+" : "") + s + " dB";
    }
}

TENSIONAudioProcessorEditor::TENSIONAudioProcessorEditor (TENSIONAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p),
      visualizer (lnf.getTheme()),
      tensionKnob (lnf.getTheme(), TensionKnob::Kind::Hero, "TENSION", percentText, 0.0, 1.0, 0.0),
      mixKnob (lnf.getTheme(), TensionKnob::Kind::Compact, "MIX", percentText, 0.0, 1.0, 1.0),
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

    auto& apvts = processorRef.getAPVTS();
    namespace P = TensionParams;

    // Sliders: SliderParameterAttachment sets the range from the parameter, brackets
    // drag gestures (Slider::startedDragging/stoppedDragging) and handles host updates.
    tensionAttach = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (P::tension), tensionKnob, nullptr);
    mixAttach     = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (P::mix), mixKnob, nullptr);
    outputAttach  = std::make_unique<juce::SliderParameterAttachment> (*apvts.getParameter (P::output), outputKnob, nullptr);

    // Double-click resets (0 % / 100 % / 0 dB). Slider's double-click path brackets the
    // gesture itself via the attachment (sliderValueChanged inside start/stop).
    tensionKnob.setDoubleClickReturnValue (true, 0.0);
    mixKnob.setDoubleClickReturnValue (true, 1.0);
    outputKnob.setDoubleClickReturnValue (true, 0.0);
    tensionKnob.setNumDecimalPlacesToDisplay (3);

    tensionKnob.onValueChange = [this] { pushVisualizerState(); };

    // Sync pill: bool param, plain value 0/1. The attachment also drives the LED from host changes.
    syncAttach = std::make_unique<juce::ParameterAttachment> (*apvts.getParameter (P::sync),
        [this] (float v) { syncPill.setOn (v > 0.5f, false); }, nullptr);
    syncPill.onChange = [this] (bool on) { syncAttach->setValueAsCompleteGesture (on ? 1.0f : 0.0f); };

    // Rise mode: ParameterAttachment drives the 3-segment pill by index (no hidden ComboBox).
    // Parameter changes (click, host automation, state restore) -> theme + visualizer gamma.
    riseAttach = std::make_unique<juce::ParameterAttachment> (*apvts.getParameter (P::riseMode),
        [this] (float v)
        {
            const int mode = juce::jlimit (0, 2, juce::roundToInt (v));
            riseSelector.setSelectedIndex (mode, false);
            applyRiseMode (mode, ! firstRiseUpdate);
            firstRiseUpdate = false;
        }, nullptr);
    riseSelector.onChange = [this] (int mode) { riseAttach->setValueAsCompleteGesture ((float) mode); };

    syncAttach->sendInitialUpdate();
    riseAttach->sendInitialUpdate();

    pushVisualizerState();

    setSize (TensionUI::kWidth, TensionUI::kHeight);   // fixed, non-resizable
    setResizable (false, false);

    lastTimerMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

TENSIONAudioProcessorEditor::~TENSIONAudioProcessorEditor()
{
    stopTimer();
    // attachments first (they reference the controls)
    tensionAttach.reset(); mixAttach.reset(); outputAttach.reset();
    syncAttach.reset(); riseAttach.reset();
    setLookAndFeel (nullptr);
}

void TENSIONAudioProcessorEditor::setPreviewState (double tensionPercent, int riseMode)
{
    // Drives the real parameters; the attachments then update the controls.
    namespace P = TensionParams;
    auto& apvts = processorRef.getAPVTS();
    apvts.getParameter (P::tension)->setValueNotifyingHost ((float) (tensionPercent / 100.0));
    apvts.getParameter (P::riseMode)->setValueNotifyingHost (
        apvts.getParameter (P::riseMode)->convertTo0to1 ((float) riseMode));
    tensionKnob.setValue (tensionPercent / 100.0, juce::dontSendNotification);
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
    s.tension = (float) tensionKnob.getValue();
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
