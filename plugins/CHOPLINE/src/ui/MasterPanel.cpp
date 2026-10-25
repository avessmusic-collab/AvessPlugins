// CHOPLINE — MasterPanel implementation. See header.

#include "MasterPanel.h"

#include "../state/Parameters.h"

namespace chopline::ui
{

using LnF = ChoplineLookAndFeel;

namespace
{
    juce::String formatHz (double hz)
    {
        return hz >= 1000.0 ? juce::String (hz / 1000.0, 1) + "k"
                            : juce::String (static_cast<int> (hz));
    }

    // MASTER – MORE flyout (v17 fl_mastermore): CEILING, DLY RTN, VERB RTN.
    class MasterMoreFlyout final : public juce::Component
    {
    public:
        explicit MasterMoreFlyout (juce::AudioProcessorValueTreeState& apvts)
        {
            title.setText ("MASTER " + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"))
                               + " MORE",
                           juce::dontSendNotification);
            title.setFont (LnF::labelFont (8.5f));
            title.setColour (juce::Label::textColourId, LnF::label);
            addAndMakeVisible (title);

            const auto setup = [&apvts, this] (CompactKnob& knob, const char* paramId,
                                               std::unique_ptr<juce::SliderParameterAttachment>& a)
            {
                addAndMakeVisible (knob);
                if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (
                        apvts.getParameter (paramId)))
                    a = std::make_unique<juce::SliderParameterAttachment> (*p, knob.getSlider());
            };
            setup (ceiling, params::id::masterCeiling, ceilingAttachment);
            ceiling.valueSuffix = " dB";
            setup (delayRtn, params::id::masterDelayRtn, delayAttachment);
            delayRtn.valueSuffix = "%";
            setup (verbRtn, params::id::masterVerbRtn, verbAttachment);
            verbRtn.valueSuffix = "%";

            setSize (190, 86);
        }

        void resized() override
        {
            title.setBounds (0, 0, getWidth(), 14);
            ceiling.setBounds (6, 22, 52, 60);
            delayRtn.setBounds (68, 22, 52, 60);
            verbRtn.setBounds (130, 22, 52, 60);
        }

    private:
        juce::Label title;
        CompactKnob ceiling { "CEILING", true }, delayRtn { "DLY RTN", true },
            verbRtn { "VERB RTN", true };
        std::unique_ptr<juce::SliderParameterAttachment> ceilingAttachment, delayAttachment,
            verbAttachment;
    };
} // namespace

void MasterPanel::LimiterLamp::paint (juce::Graphics& g)
{
    // v17 master_limiter: thin fader-style track; turns BLUE while limiting.
    const auto b = getLocalBounds().toFloat();
    auto track = juce::Rectangle<float> (4.0f, b.getHeight()).withCentre (b.getCentre());
    g.setColour (juce::Colours::black.withAlpha (0.16f));
    g.fillRoundedRectangle (track, 2.0f);

    const auto gr = juce::jlimit (0.0f, 12.0f, -grDb);
    if (gr > 0.05f)
    {
        auto fill = track.removeFromTop (track.getHeight() * (gr / 12.0f));
        g.setColour (LnF::accent); // GR meter blue (v17 faders_meters)
        g.fillRoundedRectangle (fill, 2.0f);
    }

    g.setColour (LnF::label);
    g.setFont (LnF::labelFont (6.5f));
    g.drawText ("GR", getLocalBounds().withTop (getHeight() - 8),
                juce::Justification::centred, false);
}

MasterPanel::MasterPanel (juce::AudioProcessorValueTreeState& apvts,
                          engine::Engine& engineToUse)
    : state (apvts), eng (engineToUse)
{
    namespace pid = params::id;

    const auto setup = [&apvts, this] (CompactKnob& knob, const char* paramId,
                                       std::unique_ptr<juce::SliderParameterAttachment>& a)
    {
        addAndMakeVisible (knob);
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (paramId)))
            a = std::make_unique<juce::SliderParameterAttachment> (*p, knob.getSlider());
    };

    setup (filterKnob, pid::masterFilterCutoff, filterAttachment);
    filterKnob.formatValue = [] (double v) { return formatHz (v); };
    filterKnob.getSlider().setTooltip ("Master filter cutoff (mirrors the FX page)");

    setup (distKnob, pid::masterDistDrive, distAttachment);
    distKnob.valueSuffix = "%";
    distKnob.getSlider().setTooltip ("Master distortion drive");

    setup (widthKnob, pid::masterWidth, widthAttachment);
    widthKnob.valueSuffix = "%";
    widthKnob.getSlider().setTooltip ("Stereo width");

    setup (outKnob, pid::masterOutput, outAttachment);
    outKnob.valueSuffix = " dB";
    outKnob.getSlider().setTooltip ("Output gain (before the safety limiter)");

    moreButton.setTooltip ("Ceiling + delay/reverb returns");
    moreButton.onClick = [this] { openMoreFlyout(); };
    addAndMakeVisible (moreButton);

    addAndMakeVisible (lamp);
}

void MasterPanel::openMoreFlyout()
{
    auto content = std::make_unique<MasterMoreFlyout> (state);
    juce::CallOutBox::launchAsynchronously (std::move (content),
                                            moreButton.getScreenBounds(), nullptr);
}

void MasterPanel::animationTick()
{
    const auto bpm = eng.getUiBpm();
    if (std::abs (bpm - shownBpm) > 0.049)
    {
        shownBpm = bpm;
        repaint (380, 8, 130, 24); // BPM readout region
    }

    const auto gr = eng.getMasterGainReductionDb();
    if (std::abs (gr - lamp.grDb) > 0.1f)
    {
        lamp.grDb = gr;
        lamp.repaint();
    }
}

void MasterPanel::resized()
{
    // v17 absolute positions, local to the card at (523, 593).
    filterKnob.setBounds (53, 44, 50, 54);
    distKnob.setBounds (177, 44, 50, 54);
    widthKnob.setBounds (301, 44, 50, 54);
    outKnob.setBounds (425, 44, 50, 54);
    moreButton.setBounds (510, 10, 40, 20);
    lamp.setBounds (528, 48, 22, 40);
}

void MasterPanel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (LnF::paper);
    g.fillRoundedRectangle (b, 16.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (b.reduced (0.5f), 16.0f, 1.0f);

    g.setColour (LnF::label);
    g.setFont (LnF::labelFont (8.5f));
    g.drawText ("MASTER", 10, 8, 100, 12, juce::Justification::centredLeft, false);

    // BPM readout (v17: small hairline readout at 915,603 → local 392,10).
    const auto readout = juce::Rectangle<int> (392, 10, 106, 20);
    g.setColour (LnF::display);
    g.fillRoundedRectangle (readout.toFloat(), 6.0f);
    g.setColour (LnF::hairline());
    g.drawRoundedRectangle (readout.toFloat().reduced (0.5f), 6.0f, 1.0f);
    g.setColour (LnF::textTertiary);
    g.setFont (LnF::labelFont (7.0f));
    g.drawText ("BPM", readout.reduced (8, 0), juce::Justification::centredLeft, false);
    g.setColour (LnF::ink);
    g.setFont (LnF::monoFont (9.5f));
    g.drawText (juce::String (shownBpm, 1), readout.reduced (8, 0),
                juce::Justification::centredRight, false);
}

} // namespace chopline::ui
