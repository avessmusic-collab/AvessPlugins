#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class KickDesigner2AudioProcessor;

/**
    Stage 1 native editor stub — plain resizable placeholder.
    The real premium dark modern UI (custom LookAndFeel + Knob / WaveformDisplay /
    SpectrumDisplay / PresetBrowser + section panels) is built in Stage 3.
*/
class KickDesigner2AudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit KickDesigner2AudioProcessorEditor (KickDesigner2AudioProcessor&);
    ~KickDesigner2AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    KickDesigner2AudioProcessor& processorRef;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickDesigner2AudioProcessorEditor)
};
