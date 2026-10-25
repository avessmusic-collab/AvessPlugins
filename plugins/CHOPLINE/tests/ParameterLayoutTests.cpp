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

    // Phase 2: MIDI base note + swing (slicing controls are slice-map state
    // and the pattern lives in the PATTERNS tree, per the locked v17 design).
    REQUIRE (processor.getParameters().size() == 2);

    auto* baseNote = apvts.getParameter (chopline::params::id::midiBaseNote);
    REQUIRE (baseNote != nullptr);
    REQUIRE (dynamic_cast<juce::AudioParameterChoice*> (baseNote) != nullptr);

    // Default C1; choice→note mapping is stable (Ableton naming: C1 = 36).
    const auto* choice = dynamic_cast<juce::AudioParameterChoice*> (baseNote);
    REQUIRE (choice->getIndex() == 1);
    REQUIRE (chopline::params::baseNoteForChoice (0) == 24);
    REQUIRE (chopline::params::baseNoteForChoice (1) == 36);
    REQUIRE (chopline::params::baseNoteForChoice (3) == 60);

    // Swing (spec §5): 0–75 %, default 0, automatable float.
    auto* swing = apvts.getParameter (chopline::params::id::swing);
    REQUIRE (swing != nullptr);
    const auto* swingFloat = dynamic_cast<juce::AudioParameterFloat*> (swing);
    REQUIRE (swingFloat != nullptr);
    REQUIRE (juce::exactlyEqual (swingFloat->get(), 0.0f));
    REQUIRE (juce::exactlyEqual (swingFloat->getNormalisableRange().start, 0.0f));
    REQUIRE (juce::exactlyEqual (swingFloat->getNormalisableRange().end, 75.0f));

    // State tree exists and carries the right root type for save/recall.
    REQUIRE (apvts.state.isValid());
    REQUIRE (apvts.state.getType() == juce::Identifier ("CHOPLINE"));

    // Schema version constant is wired up (spec §10: versioned from day one).
    REQUIRE (chopline::params::kSchemaVersion == 1);
}
