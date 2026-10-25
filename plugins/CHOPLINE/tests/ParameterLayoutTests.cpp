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

    // Phase 2: MIDI base note + swing. Phase 4 adds the FULL Beat Repeat set
    // (17, spec §7 — all automatable) and the master chain (23). Slicing
    // controls stay slice-map state per the locked v17 design.
    REQUIRE (processor.getParameters().size() == 42);

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

TEST_CASE ("Phase 4 parameters: defaults match the locked v17 design", "[state][parameters]")
{
    namespace pid = chopline::params::id;
    juce::ScopedJuceInitialiser_GUI juceInit;

    DummyProcessor processor;
    juce::AudioProcessorValueTreeState apvts (processor, nullptr,
                                              juce::Identifier ("CHOPLINE"),
                                              chopline::params::createParameterLayout());

    const auto choiceIndex = [&apvts] (const char* id)
    {
        auto* c = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id));
        REQUIRE (c != nullptr);
        return c->getIndex();
    };
    const auto floatValue = [&apvts] (const char* id)
    {
        auto* f = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (id));
        REQUIRE (f != nullptr);
        return f->get();
    };
    const auto boolValue = [&apvts] (const char* id)
    {
        auto* b = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (id));
        REQUIRE (b != nullptr);
        return b->get();
    };

    // Beat Repeat (v17 bottom bar + not_surfaced_in_mockup defaults).
    REQUIRE (choiceIndex (pid::brMode) == 1);      // Manual
    REQUIRE_FALSE (boolValue (pid::brHold));
    REQUIRE (choiceIndex (pid::brInterval) == 1);  // 1/2 bar
    REQUIRE (choiceIndex (pid::brOffset) == 0);
    REQUIRE (juce::exactlyEqual (floatValue (pid::brChance), 60.0f));
    REQUIRE (choiceIndex (pid::brGrid) == 3);      // 1/16
    REQUIRE (choiceIndex (pid::brGate) == 3);      // 1/2 bar
    REQUIRE (juce::exactlyEqual (floatValue (pid::brDecay), 25.0f));
    REQUIRE (juce::exactlyEqual (floatValue (pid::brPitchDecay), 0.0f));
    REQUIRE (choiceIndex (pid::brReverse) == 0);
    REQUIRE (choiceIndex (pid::brFilterMode) == 0);
    REQUIRE (choiceIndex (pid::brMix) == 0);       // Mix
    REQUIRE_FALSE (boolValue (pid::brFreeze));
    REQUIRE (choiceIndex (pid::brMidiBase) == 1);  // C3 = 60 (spec §7)
    REQUIRE (chopline::params::brBaseNoteForChoice (1) == 60);

    // Master chain (v17 defaults).
    REQUIRE (boolValue (pid::masterFilterOn));
    REQUIRE (choiceIndex (pid::masterFilterType) == 0); // LP
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterFilterCutoff), 20000.0f));
    REQUIRE (boolValue (pid::masterDistOn));
    REQUIRE (choiceIndex (pid::masterDistType) == 2);   // Tape
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterDistDrive), 10.0f));
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterDistMix), 100.0f));
    REQUIRE (boolValue (pid::masterDistAutoGain));
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterWidth), 100.0f));
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterMono), 120.0f));
    REQUIRE (boolValue (pid::masterLimiterOn));
    REQUIRE (std::abs (floatValue (pid::masterOutput)) < 1.0e-3f);
    REQUIRE (std::abs (floatValue (pid::masterCeiling) - -0.3f) < 1.0e-3f);
    REQUIRE (choiceIndex (pid::masterDelayTime) == 2);  // 1/8.
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterDelayFb), 35.0f));
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterDelayRtn), 0.0f));
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterVerbDecay), 40.0f));
    REQUIRE (juce::exactlyEqual (floatValue (pid::masterVerbRtn), 0.0f));
}

TEST_CASE ("Phase 4 parameters: state round trip + older states load", "[state][parameters]")
{
    namespace pid = chopline::params::id;
    juce::ScopedJuceInitialiser_GUI juceInit;

    DummyProcessor processorA, processorB;
    juce::AudioProcessorValueTreeState a (processorA, nullptr, juce::Identifier ("CHOPLINE"),
                                          chopline::params::createParameterLayout());
    juce::AudioProcessorValueTreeState bState (processorB, nullptr, juce::Identifier ("CHOPLINE"),
                                               chopline::params::createParameterLayout());

    // Non-default values on a spread of every new parameter kind.
    const auto set = [&a] (const char* id, float normalised)
    {
        auto* p = a.getParameter (id);
        REQUIRE (p != nullptr);
        p->setValueNotifyingHost (normalised);
    };
    set (pid::brHold, 1.0f);
    set (pid::brMode, 0.0f);          // Auto
    set (pid::brGrid, 1.0f);          // 1/32T
    set (pid::brPitchDecay, 0.5f);
    set (pid::masterFilterType, 1.0f); // HP
    set (pid::masterDistType, 1.0f);   // Downsample
    set (pid::masterWidth, 1.0f);      // 200 %
    set (pid::masterCeiling, 0.0f);    // −12 dB
    set (pid::masterDelayRtn, 0.7f);
    set (pid::masterVerbRtn, 0.3f);
    set (pid::masterLimiterOn, 0.0f);

    // Round trip through XML (the processor path uses copyState/replaceState).
    const auto xml = a.copyState().createXml();
    REQUIRE (xml != nullptr);
    bState.replaceState (juce::ValueTree::fromXml (*xml));

    for (auto* p : processorA.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        REQUIRE (ranged != nullptr);
        auto* mirror = bState.getParameter (ranged->getParameterID());
        REQUIRE (mirror != nullptr);
        REQUIRE (std::abs (mirror->getValue() - ranged->getValue()) < 1.0e-6f);
    }

    // A Phase 1/2/3-shaped state (no Phase 4 parameter children) still loads:
    // the new parameters keep their current (default) values.
    juce::ValueTree oldState ("CHOPLINE");
    oldState.setProperty ("schemaVersion", 1, nullptr);
    {
        juce::ValueTree swingParam ("PARAM");
        swingParam.setProperty ("id", "swing", nullptr);
        swingParam.setProperty ("value", 30.0, nullptr);
        oldState.appendChild (swingParam, nullptr);
    }
    DummyProcessor processorC;
    juce::AudioProcessorValueTreeState c (processorC, nullptr, juce::Identifier ("CHOPLINE"),
                                          chopline::params::createParameterLayout());
    c.replaceState (oldState);
    auto* swing = dynamic_cast<juce::AudioParameterFloat*> (c.getParameter (pid::swing));
    REQUIRE (swing != nullptr);
    REQUIRE (juce::exactlyEqual (swing->get(), 30.0f));
    auto* ceiling = dynamic_cast<juce::AudioParameterFloat*> (c.getParameter (pid::masterCeiling));
    REQUIRE (ceiling != nullptr);
    REQUIRE (std::abs (ceiling->get() - -0.3f) < 1.0e-3f); // default survives
}
