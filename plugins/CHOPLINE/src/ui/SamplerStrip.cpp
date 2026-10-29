// CHOPLINE — SamplerStrip implementation. See header.

#include "SamplerStrip.h"

namespace chopline::ui
{

namespace
{
    const juce::StringArray audioExtensions { ".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg" };

    bool isAudioFile (const juce::String& path)
    {
        for (const auto& ext : audioExtensions)
            if (path.endsWithIgnoreCase (ext))
                return true;
        return false;
    }

    // ---- '…' slicing flyout (v17 fl_slice: MIN LEN, Snap zero-X, + sample BPM) ----
    class SliceFlyout final : public juce::Component
    {
    public:
        explicit SliceFlyout (engine::Engine& eng) : engine (eng)
        {
            title.setText ("SLICING", juce::dontSendNotification);
            title.setFont (ChoplineLookAndFeel::labelFont (8.5f));
            title.setColour (juce::Label::textColourId, ChoplineLookAndFeel::label);
            addAndMakeVisible (title);

            minLen.getSlider().setRange (20.0, 500.0, 1.0);
            minLen.valueSuffix = " ms";
            minLen.setValueQuiet (engine.getMinSliceLengthMs());
            minLen.onValueChange = [this] (double v)
            {
                engine.setMinSliceLengthMs (static_cast<float> (v));
            };
            addAndMakeVisible (minLen);

            zeroSnap.setButtonText ("Snap zero-X");
            zeroSnap.setToggleState (engine.getZeroCrossSnap(), juce::dontSendNotification);
            zeroSnap.onClick = [this] { engine.setZeroCrossSnap (zeroSnap.getToggleState()); };
            addAndMakeVisible (zeroSnap);

            bpmCaption.setText ("SAMPLE BPM", juce::dontSendNotification);
            bpmCaption.setFont (ChoplineLookAndFeel::labelFont (8.5f));
            bpmCaption.setColour (juce::Label::textColourId, ChoplineLookAndFeel::label);
            addAndMakeVisible (bpmCaption);

            bpmValue.setEditable (true);
            bpmValue.setFont (ChoplineLookAndFeel::monoFont (10.0f));
            bpmValue.setColour (juce::Label::textColourId, ChoplineLookAndFeel::ink);
            bpmValue.setJustificationType (juce::Justification::centredRight);
            bpmValue.setText (juce::String (engine.getSampleBpm(), 1), juce::dontSendNotification);
            bpmValue.onTextChange = [this]
            {
                const auto v = bpmValue.getText().getDoubleValue();
                if (v > 1.0)
                    engine.setSampleBpm (v);
                bpmValue.setText (juce::String (engine.getSampleBpm(), 1), juce::dontSendNotification);
            };
            addAndMakeVisible (bpmValue);

            setSize (220, 118);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            title.setBounds (b.removeFromTop (14));
            b.removeFromTop (4);
            minLen.setBounds (b.removeFromTop (32));
            b.removeFromTop (6);
            zeroSnap.setBounds (b.removeFromTop (24));
            b.removeFromTop (6);
            auto row = b.removeFromTop (20);
            bpmCaption.setBounds (row.removeFromLeft (110));
            bpmValue.setBounds (row);
        }

    private:
        engine::Engine& engine;
        juce::Label title;
        CompactKnob minLen { "MIN LEN" };
        juce::ToggleButton zeroSnap;
        juce::Label bpmCaption, bpmValue;
    };
} // namespace

SamplerStrip::SamplerStrip (engine::Engine& engineToUse)
    : engine (engineToUse), waveform (engineToUse)
{
    addAndMakeVisible (waveform);

    loadButton.onClick = [this] { openLoadChooser(); };
    addAndMakeVisible (loadButton);

    moreButton.onClick = [this] { openSliceFlyout(); };
    addAndMakeVisible (moreButton);

    modeBox.setName ("SLICE"); // drawn as the tiny uppercase prefix
    modeBox.addItem ("Transient", 1);
    modeBox.addItem ("Grid", 2);
    modeBox.addItem ("Equal", 3);
    modeBox.addItem ("Manual", 4);
    modeBox.onChange = [this]
    {
        engine.setSliceMode (static_cast<engine::SliceMode> (modeBox.getSelectedId() - 1));
    };
    addAndMakeVisible (modeBox);

    sensKnob.getSlider().setRange (0.0, 100.0, 1.0);
    sensKnob.valueSuffix = "%";
    sensKnob.onValueChange = [this] (double v)
    {
        engine.setSensitivity (static_cast<float> (v) / 100.0f);
    };
    sensKnob.setTooltip ("Slice control: sensitivity (Transient), division (Grid), count (Equal)");
    addAndMakeVisible (sensKnob);

    syncControlsFromEngine();
    engine.addChangeListener (this);
}

SamplerStrip::~SamplerStrip()
{
    engine.removeChangeListener (this);
}

void SamplerStrip::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncControlsFromEngine();
    waveform.modelChanged();
    engine.collectGarbage();
}

void SamplerStrip::syncControlsFromEngine()
{
    modeBox.setSelectedId (static_cast<int> (engine.getSliceMode()) + 1,
                           juce::dontSendNotification);
    sensKnob.setValueQuiet (static_cast<double> (engine.getSensitivity()) * 100.0);
}

void SamplerStrip::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (ChoplineLookAndFeel::paper);
    g.fillRoundedRectangle (b, 16.0f);
    g.setColour (ChoplineLookAndFeel::hairline());
    g.drawRoundedRectangle (b.reduced (0.5f), 16.0f, 1.0f);

    if (dropHighlight)
    {
        g.setColour (ChoplineLookAndFeel::accent);
        g.drawRoundedRectangle (b.reduced (1.5f), 15.0f, 1.5f);
    }
}

void SamplerStrip::resized()
{
    // v17 strip_a geometry relative to the card at (15, 60): waveform 12,12
    // 830x114; Load 858,12 148x28; '…' 1016,12 42x28; SLICE combo 858,50
    // 200x28; SENS 858,88 140x40.
    waveform.setBounds (12, 12, 830, 114);
    loadButton.setBounds (858, 12, 148, 28);
    moreButton.setBounds (1016, 12, 42, 28);
    modeBox.setBounds (858, 50, 200, 28);
    sensKnob.setBounds (858, 88, 150, 38);
}

void SamplerStrip::openLoadChooser()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Load a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file.existsAsFile())
            engine.loadFile (file);
    });
}

void SamplerStrip::openSliceFlyout()
{
    auto content = std::make_unique<SliceFlyout> (engine);
    juce::CallOutBox::launchAsynchronously (std::move (content),
                                            moreButton.getScreenBounds(),
                                            nullptr);
}

// ---- file drag-and-drop -----------------------------------------------------------

bool SamplerStrip::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void SamplerStrip::filesDropped (const juce::StringArray& files, int, int)
{
    dropHighlight = false;
    repaint();
    for (const auto& f : files)
    {
        if (isAudioFile (f))
        {
            engine.loadFile (juce::File (f));
            return;
        }
    }
}

void SamplerStrip::fileDragEnter (const juce::StringArray&, int, int)
{
    dropHighlight = true;
    repaint();
}

void SamplerStrip::fileDragExit (const juce::StringArray&)
{
    dropHighlight = false;
    repaint();
}

} // namespace chopline::ui
