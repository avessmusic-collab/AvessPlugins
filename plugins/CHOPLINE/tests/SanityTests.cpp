// CHOPLINE tests — build/runtime sanity (Phase 0).

#include <catch2/catch_test_macros.hpp>

#include "engine/Engine.h"

TEST_CASE ("Sanity: test runner executes", "[sanity]")
{
    REQUIRE (1 + 1 == 2);
}

TEST_CASE ("Engine stub prepares and renders silence", "[engine]")
{
    chopline::engine::Engine engine;
    engine.prepare (48000.0, 512, 2);
    REQUIRE (juce::exactlyEqual (engine.getSampleRate(), 48000.0));

    juce::AudioBuffer<float> buffer (2, 512);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample (ch, i, 1.0f); // garbage in

    juce::MidiBuffer midi;
    engine.process (buffer, midi);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        REQUIRE (juce::exactlyEqual (buffer.getMagnitude (ch, 0, buffer.getNumSamples()), 0.0f));
}
