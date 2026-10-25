// CHOPLINE tests — PatternScheduler through the Engine (spec §8, §12):
// exact-sample triggers for all 9 note lengths, swing on even steps, tempo
// changes mid-run, loop wraps (PPQ discontinuities) releasing only pattern
// voices, transport stop vs. MIDI, free-run fallbacks, block sizes 1–4096,
// and pattern state round trips (including Phase 1-shaped state).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "TestHelpers.h"

#include <cmath>

using namespace chopline;
using namespace chopline::test;

namespace
{
    constexpr double rate = 48000.0;

    void loadEightChops (engine::Engine& eng, int maxBlock = 4096)
    {
        eng.prepare (rate, maxBlock, 2);
        REQUIRE (eng.loadBufferSync (makeStaircase (8, 2.0, rate), rate).isEmpty());
        eng.setZeroCrossSnap (false);
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (6.0f / 62.0f); // → exactly 8 equal slices
        REQUIRE (eng.getNumChops() == 8);
    }

    void loadTwoLongChops (engine::Engine& eng) // 2 chops of 2 s each
    {
        eng.prepare (rate, 4096, 2);
        REQUIRE (eng.loadBufferSync (makeStaircase (2, 4.0, rate), rate).isEmpty());
        eng.setZeroCrossSnap (false);
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (0.0f); // → 2 slices
        REQUIRE (eng.getNumChops() == 2);
    }
} // namespace

TEST_CASE ("Pattern triggers on the exact sample for all 9 note lengths",
           "[scheduler][resolution]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto nl = GENERATE (range (0, engine::numNoteLengths));
    DYNAMIC_SECTION ("note length " << engine::noteLengthLabel (static_cast<engine::NoteLength> (nl)))
    {
        engine::Engine eng;
        loadEightChops (eng);

        auto& pm = eng.getPatternModel();
        pm.setNoteLength (0, static_cast<engine::NoteLength> (nl));
        const auto stepLen = engine::noteLengthBars (static_cast<engine::NoteLength> (nl));

        // Event on step 1 (the second box).
        REQUIRE (pm.addEvent (0, stepLen, stepLen, 1) >= 0);

        SimHost host;
        const auto out = renderWithHost (eng, host, 48000, 512);
        const auto expected = static_cast<int> (std::llround (stepLen * host.samplesPerBar()));
        REQUIRE (firstAudibleSample (out) == expected);
    }
}

TEST_CASE ("Swing delays even steps (odd 0-based indices) by swing% of a step",
           "[scheduler][swing]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto stepSamples = 6000.0; // 1/16 at 120 BPM, 48 kHz

    SECTION ("odd step index is swung")
    {
        engine::Engine eng;
        loadEightChops (eng);
        eng.getPatternModel().addEvent (0, 1.0 / 16.0, 1.0 / 16.0, 1); // step 1

        SimHost host;
        host.swingPct = 50.0f;
        const auto out = renderWithHost (eng, host, 24000, 512);
        REQUIRE (firstAudibleSample (out) == static_cast<int> (stepSamples * 1.5)); // 9000
    }

    SECTION ("even step index is untouched")
    {
        engine::Engine eng;
        loadEightChops (eng);
        eng.getPatternModel().addEvent (0, 2.0 / 16.0, 1.0 / 16.0, 1); // step 2

        SimHost host;
        host.swingPct = 50.0f;
        const auto out = renderWithHost (eng, host, 24000, 512);
        REQUIRE (firstAudibleSample (out) == static_cast<int> (stepSamples * 2.0)); // 12000
    }

    SECTION ("maximum swing is 75% of a step")
    {
        engine::Engine eng;
        loadEightChops (eng);
        eng.getPatternModel().addEvent (0, 1.0 / 16.0, 1.0 / 16.0, 1);

        SimHost host;
        host.swingPct = 75.0f;
        const auto out = renderWithHost (eng, host, 24000, 512);
        REQUIRE (firstAudibleSample (out) == static_cast<int> (stepSamples * 1.75)); // 10500
    }
}

TEST_CASE ("Tempo change mid-run: the next trigger lands on the PPQ-exact sample",
           "[scheduler][tempo]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadEightChops (eng);
    eng.getPatternModel().addEvent (0, 0.0, 1.0 / 16.0, 1); // step 0, every bar

    SimHost host;
    juce::AudioBuffer<float> full (2, 120000);
    full.clear();
    juce::AudioBuffer<float> block (2, 512);
    const juce::MidiBuffer noMidi;

    bool switched = false;
    int expectedSecondOnset = -1;
    for (int pos = 0; pos + 512 <= 120000; pos += 512)
    {
        if (! switched && host.ppq >= 3.2)
        {
            host.bpm = 160.0; // tempo jumps mid-bar
            switched = true;
        }
        const auto blockStartPpq = host.ppq;
        eng.process (block, noMidi, host.ctx());
        host.advance (512);
        if (blockStartPpq < 4.0 && host.ppq >= 4.0) // bar 2 starts inside this block
            expectedSecondOnset = pos + static_cast<int> (
                std::llround ((4.0 - blockStartPpq) / 4.0 * host.samplesPerBar()));
        for (int ch = 0; ch < 2; ++ch)
            full.copyFrom (ch, pos, block, ch, 0, 512);
    }

    REQUIRE (switched);
    REQUIRE (expectedSecondOnset > 0);
    REQUIRE (firstAudibleSample (full) == 0);
    // The chop (12000 samples) has long finished before the bar wraps.
    REQUIRE (firstAudibleSample (full, 13000) == expectedSecondOnset);
}

TEST_CASE ("Loop wrap (PPQ discontinuity) releases only pattern voices and retriggers",
           "[scheduler][loop]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoLongChops (eng);
    eng.getPatternModel().addEvent (0, 0.0, 1.0, 0); // chop 0 (2 s) at step 0

    SimHost host;
    auto out = renderWithHost (eng, host, 24000, 512); // 0.5 s in: voice is ringing
    REQUIRE (firstAudibleSample (out) == 0);
    REQUIRE (eng.countActiveVoices() == 1);

    // A held MIDI chop rings alongside the pattern.
    {
        juce::AudioBuffer<float> block (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 37, static_cast<juce::uint8> (100)), 0);
        eng.process (block, midi, host.ctx());
        host.advance (512);
    }
    REQUIRE (eng.countActiveVoices() == 2);

    // The host loops back to the pattern start.
    host.ppq = 0.0;
    {
        juce::AudioBuffer<float> block (2, 512);
        const juce::MidiBuffer noMidi;
        eng.process (block, noMidi, host.ctx());
        host.advance (512);
        // The wrap retriggers step 0 at the block start.
        REQUIRE (std::abs (block.getSample (0, 256)) > 0.01f);
    }

    // Old pattern voice: 2 ms fade → dead. MIDI voice + new pattern voice ring on.
    REQUIRE (eng.countActiveVoices() == 2);
}

TEST_CASE ("Transport stop releases pattern voices; MIDI chops still play",
           "[scheduler][transport]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadTwoLongChops (eng);
    eng.getPatternModel().addEvent (0, 0.0, 1.0, 0);

    SimHost host;
    renderWithHost (eng, host, 24000, 512);
    REQUIRE (eng.countActiveVoices() == 1);

    host.playing = false;
    juce::AudioBuffer<float> block (2, 512);
    const juce::MidiBuffer noMidi;
    for (int i = 0; i < 3; ++i)
        eng.process (block, noMidi, host.ctx()); // stopped: PPQ frozen
    REQUIRE (eng.countActiveVoices() == 0); // released with the short fade

    // MIDI triggering keeps working while the transport is stopped (spec §8).
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (110)), 10);
    eng.process (block, midi, host.ctx());
    REQUIRE (eng.countActiveVoices() == 1);
    REQUIRE (std::abs (block.getSample (0, 300)) > 0.01f);

    // Restarting the transport resumes the pattern at the right step.
    host.playing = true;
    host.ppq = 0.0;
    const auto out = renderWithHost (eng, host, 4800, 512);
    REQUIRE (firstAudibleSample (out) == 0);
}

TEST_CASE ("Exact triggers across block sizes 1–4096", "[scheduler][blocks]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto blockSize = GENERATE (1, 3, 16, 333, 512, 4096);
    DYNAMIC_SECTION ("block size " << blockSize)
    {
        engine::Engine eng;
        loadEightChops (eng, 4096);
        eng.getPatternModel().addEvent (0, 3.0 / 16.0, 1.0 / 16.0, 1); // step 3

        SimHost host;
        const auto out = renderWithHost (eng, host, 20000, blockSize);
        REQUIRE (firstAudibleSample (out) == 18000);
        for (int i = 0; i < out.getNumSamples(); ++i)
            REQUIRE (std::isfinite (out.getSample (0, i)));
    }
}

TEST_CASE ("Host without PPQ: internal accumulator at the host BPM",
           "[scheduler][fallback]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadEightChops (eng);
    eng.getPatternModel().addEvent (0, 0.0, 1.0 / 16.0, 1);

    SimHost host;
    host.hasPpq = false; // playing, but the host reports no PPQ
    const auto out = renderWithHost (eng, host, 100000, 512);
    REQUIRE (firstAudibleSample (out) == 0);
    // The chop (12000 samples) ends; the pattern wraps a bar later (96000).
    REQUIRE (firstAudibleSample (out, 20000) == 96000);

    // The UI playhead runs while the pattern plays (position within the pattern).
    REQUIRE (eng.getUiPatternPositionBars() >= 0.0f);
    REQUIRE (eng.getUiPatternPositionBars() < 1.0f);
}

TEST_CASE ("Standalone (no playhead): free-run at the manual BPM",
           "[scheduler][standalone]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadEightChops (eng);

    auto& pm = eng.getPatternModel();
    pm.setFreeBpm (60.0);
    pm.setNoteLength (0, engine::NoteLength::quarter);
    pm.addEvent (0, 0.25, 0.25, 1); // step 1 of 1/4

    SimHost host;
    host.hasPosition = false;
    host.hasPpq = false;
    host.hasBpm = false;
    const auto out = renderWithHost (eng, host, 60000, 512);
    // 60 BPM, 4/4: one bar = 192000 samples; step 1 of 1/4 = 48000.
    REQUIRE (firstAudibleSample (out) == 48000);
}

TEST_CASE ("Pattern state round trip is identical (spec §12)", "[scheduler][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    engine::Engine engA;
    loadEightChops (engA);
    auto& pm = engA.getPatternModel();
    pm.setNoteLength (0, engine::NoteLength::eighthTriplet);
    pm.setPatternBars (0, 2);
    pm.addEvent (0, 0.0, 1.0 / 12.0, 3, 90);
    pm.addEvent (0, 1.0 / 12.0, 3.0 / 12.0, 5, 127); // spanned event
    pm.addEvent (0, 1.25, 1.0 / 12.0, 1, 40);        // second bar
    pm.addEvent (3, 0.5, 1.0 / 16.0, 2);             // another slot (serialized too)
    pm.setFreeBpm (97.5);

    juce::ValueTree rootA ("CHOPLINE");
    engA.serializeTo (rootA);

    engine::Engine engB;
    engB.prepare (rate, 512, 2);
    engB.restoreFrom (rootA, true);

    juce::ValueTree rootB ("CHOPLINE");
    engB.serializeTo (rootB);

    const auto patternsA = rootA.getChildWithName (state::PatternModel::patternsTag);
    const auto patternsB = rootB.getChildWithName (state::PatternModel::patternsTag);
    REQUIRE (patternsA.isValid());
    REQUIRE (patternsB.isValid());
    REQUIRE (patternsA.isEquivalentTo (patternsB));

    // And the restored model actually plays: same first trigger as the source.
    REQUIRE (engB.getPatternModel().getNumEvents (0) == 3);
    REQUIRE (engB.getPatternModel().getPatternBars (0) == 2);
    SimHost host;
    const auto out = renderWithHost (engB, host, 2000, 512);
    REQUIRE (firstAudibleSample (out) == 0);
}

TEST_CASE ("Phase 1-shaped state (no PATTERNS child) loads with default patterns",
           "[scheduler][state][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Serialize a Phase 1-style state: sample + slices, NO patterns child.
    engine::Engine engA;
    loadEightChops (engA);
    juce::ValueTree root ("CHOPLINE");
    root.setProperty ("schemaVersion", 1, nullptr);
    engA.serializeTo (root);
    root.removeChild (root.getChildWithName (state::PatternModel::patternsTag), nullptr);
    REQUIRE (! root.getChildWithName (state::PatternModel::patternsTag).isValid());

    // Restore into an engine that already carries pattern edits.
    engine::Engine engB;
    engB.prepare (rate, 512, 2);
    engB.getPatternModel().addEvent (0, 0.0, 0.25, 1);
    engB.getPatternModel().setPatternBars (0, 4);
    engB.restoreFrom (root, true);

    auto& pm = engB.getPatternModel();
    REQUIRE (pm.getNumEvents (0) == 0);
    REQUIRE (pm.getPatternBars (0) == 1);
    REQUIRE (pm.getNoteLength (0) == engine::NoteLength::sixteenth);
    REQUIRE (engB.getNumChops() == 8); // the Phase 1 payload still loads

    // And it still renders (MIDI path intact, no pattern triggers).
    SimHost host;
    const auto out = renderWithHost (engB, host, 9600, 512);
    REQUIRE (firstAudibleSample (out) == -1);
}
