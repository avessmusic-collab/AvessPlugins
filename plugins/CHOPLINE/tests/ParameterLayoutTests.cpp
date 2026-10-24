// CHOPLINE tests — APVTS parameter layout (Phase 0: layout is empty).

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include "state/Parameters.h"

namespace
{

// Minimal headless processor so the APVTS can be constructed in a console
// test with no plugin wrapper or editor linked.
class DummyProcessor final : public juce::AudioProcessor
{
public:
    DummyProcessor()
        : AudioProcessor (BusesProperties()
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.clear(); }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    const juce::String getName() const override { return "CHOPLINE-DummyProcessor"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

} // namespace

TEST_CASE ("Parameter layout instantiates and builds an APVTS", "[state][parameters]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    DummyProcessor processor;
    juce::AudioProcessorValueTreeState apvts (processor, nullptr,
                                              juce::Identifier ("CHOPLINE"),
                                              chopline::params::createParameterLayout());

    // Phase 0: the layout is intentionally empty.
    REQUIRE (processor.getParameters().size() == 0);

    // State tree exists and carries the right root type for save/recall.
    REQUIRE (apvts.state.isValid());
    REQUIRE (apvts.state.getType() == juce::Identifier ("CHOPLINE"));

    // Schema version constant is wired up (spec §10: versioned from day one).
    REQUIRE (chopline::params::kSchemaVersion == 1);
}
