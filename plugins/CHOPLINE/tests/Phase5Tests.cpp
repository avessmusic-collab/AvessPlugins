// CHOPLINE tests — Phase 5 (spec §5/§6/§8/§10/§12):
//   per-step lanes (pitch through every warp engine, ratchet counts + exact
//   timing at block sizes 1–4096, probability 0/100 + seeded determinism),
//   Beat Repeat AUTO chance determinism, the seeded generators, bar-quantised
//   pattern-slot switching, granular/freeze (no NaN, event-length fill,
//   declick-clean), the preset system (full vs kit-only, category scan,
//   mismatched chop mapping), drag-out MIDI parse-back, offline render
//   determinism (bounce == bounce) and Eco vs HQ cleanliness.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"

#include "state/Exporter.h"
#include "state/Parameters.h"
#include "state/PresetManager.h"

#include <cmath>

using Catch::Approx;

using namespace chopline;
using namespace chopline::test;

namespace
{
    constexpr double rate = 48000.0;

    /// Frequency from the MEDIAN period between positive-going zero crossings.
    /// Robust to the handful of phase-jump cycles at granular grain boundaries
    /// (the in-grain pitch is what the 2-cent spec §12 bound is about).
    double estimateFreq (const juce::AudioBuffer<float>& buf, int start, int end)
    {
        const auto* d = buf.getReadPointer (0);
        std::vector<double> periods;
        double prev = -1.0;
        for (int i = start + 1; i < juce::jmin (end, buf.getNumSamples()); ++i)
        {
            if (d[i - 1] < 0.0f && d[i] >= 0.0f)
            {
                const auto frac = static_cast<double> (d[i - 1])
                                / (static_cast<double> (d[i - 1]) - static_cast<double> (d[i]));
                const auto t = static_cast<double> (i - 1) + frac;
                if (prev >= 0.0)
                    periods.push_back (t - prev);
                prev = t;
            }
        }
        if (periods.empty())
            return 0.0;
        std::sort (periods.begin(), periods.end());
        return rate / periods[periods.size() / 2];
    }

    double centsOff (double freq, double target)
    {
        return 1200.0 * std::log2 (freq / target);
    }

    /// 2 s sine → Equal mode, 2 chops of ~1 s each; choke off, jitters zeroed.
    void loadSineChops (engine::Engine& eng)
    {
        eng.prepare (rate, 512, 2);
        REQUIRE (eng.loadBufferSync (makeSine (440.0, 2.0, rate), rate).isEmpty());
        eng.setZeroCrossSnap (false);
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (0.0f); // → 2 slices
        REQUIRE (eng.getNumChops() == 2);
        disableChoke (eng);
        for (int i = 0; i < 2; ++i)
        {
            auto s = eng.getChopSettings (i);
            s.grainJitterMs = 0.0f;         // deterministic pure-tone grains
            s.grainPitchJitterCents = 0.0f;
            s.grainSprayMs = 0.0f;
            eng.setChopSettings (i, s);
        }
    }

    juce::ValueTree serializeEngine (engine::Engine& eng)
    {
        juce::ValueTree root ("CHOPLINE");
        root.setProperty ("schemaVersion", params::kSchemaVersion, nullptr);
        eng.serializeTo (root);
        return root;
    }
} // namespace

// ---- per-step PITCH lane through every warp engine (spec §5/§12) -----------------------

TEST_CASE ("Pitch lane: +7 st lands within 2 cents through every warp engine",
           "[phase5][lanes][pitch]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    struct Case { engine::WarpMode warp; const char* name; double tolCents; };
    const Case cases[] = {
        { engine::WarpMode::varispeed, "varispeed", 2.0 },
        { engine::WarpMode::stretch, "stretch", 2.0 },
        { engine::WarpMode::granular, "granular", 2.0 },
        { engine::WarpMode::freeze, "freeze", 2.0 },
    };
    const auto target = 440.0 * std::pow (2.0, 7.0 / 12.0);

    for (const auto& c : cases)
    {
        INFO ("warp engine: " << c.name);
        engine::Engine eng;
        loadSineChops (eng);
        auto s = eng.getChopSettings (0);
        s.warp = c.warp;
        s.mode = engine::PlaybackMode::gate;
        if (c.warp == engine::WarpMode::granular)
        {
            // Grain-level pitch accuracy: non-overlapping grains isolate the
            // per-grain transpose. (Overlapping zero-jitter grains add a
            // coherent phase-slip detune — an inherent granular artifact that
            // the default position jitter randomises away; documented.)
            s.grainMs = 200.0f;
            s.grainDensity = 1;
        }
        eng.setChopSettings (0, s);

        auto& pm = eng.getPatternModel();
        const auto id = pm.addEvent (0, 0.0, 0.5, 0, 127); // half a bar = 1 s at 120
        REQUIRE (id >= 0);
        REQUIRE (pm.setEventPitch (0, id, 7.0f));

        SimHost host;
        const auto out = renderWithHost (eng, host, 48000, 512);
        const auto freq = estimateFreq (out, 8000, 40000);
        INFO ("measured " << freq << " Hz, target " << target);
        REQUIRE (freq > 0.0);
        REQUIRE (std::abs (centsOff (freq, target)) < c.tolCents);
    }
}

// ---- RATCHET: counts + exact timing at block sizes 1–4096 (spec §5/§12) ----------------

TEST_CASE ("Ratchet subdivides the step exactly, with 0.8x decay, at any block size",
           "[phase5][lanes][ratchet]")
{
    engine::PatternSnapshot snap;
    auto& slot = snap.slots[0];
    slot.bars = 1;
    slot.noteLength = engine::NoteLength::quarter;
    slot.numEvents = 1;
    slot.events[0] = { 0.0, 0.25, 3, 1.0f, 0.0f, 100, 4, false }; // 1/4 span, ratchet 4

    const auto samplesPerBar = rate * 60.0 / 120.0 * 4.0; // 96000
    const auto subLen = 0.25 / 4.0 * samplesPerBar;       // 6000

    for (const int blockSize : { 1, 16, 100, 256, 511, 1024, 4096 })
    {
        INFO ("block size " << blockSize);
        engine::PatternScheduler sched;
        sched.prepare (rate);

        engine::PatternScheduler::HostView host;
        host.hasPosition = true;
        host.playing = true;
        host.hasPpq = true;
        host.hasBpm = true;
        host.bpm = 120.0;

        std::vector<std::pair<juce::int64, float>> fired; // (absolute sample, velocity)
        juce::int64 pos = 0;
        const auto total = static_cast<juce::int64> (samplesPerBar * 2.0); // 2 bars
        while (pos < total)
        {
            const auto n = static_cast<int> (juce::jmin<juce::int64> (blockSize, total - pos));
            host.ppq = static_cast<double> (pos) / rate * 2.0; // 120 BPM: 2 q/s
            engine::PatternScheduler::Trigger trig[engine::PatternScheduler::maxTriggers];
            bool release = false;
            const auto count = sched.process (&snap, host, 0.0f, 120.0, n, 0, true,
                                              trig, release);
            for (int i = 0; i < count; ++i)
                fired.emplace_back (pos + trig[i].offset, trig[i].velocity);
            pos += n;
        }

        REQUIRE (fired.size() == 8); // 4 ratchets × 2 bars
        for (int bar = 0; bar < 2; ++bar)
        {
            for (int k = 0; k < 4; ++k)
            {
                const auto& f = fired[static_cast<size_t> (bar * 4 + k)];
                const auto expected = static_cast<juce::int64> (std::llround (
                    static_cast<double> (bar) * samplesPerBar + k * subLen));
                REQUIRE (f.first == expected); // exact-sample timing
                REQUIRE (f.second == Approx (std::pow (0.8f, static_cast<float> (k))));
            }
        }
    }
}

// ---- PROBABILITY: 0/100 and seeded determinism (spec §5/§12) ---------------------------

TEST_CASE ("Probability: 0% never fires, 100% always, same seed+position = same fires",
           "[phase5][lanes][probability]")
{
    engine::PatternSnapshot snap;
    auto& slot = snap.slots[0];
    slot.bars = 1;
    slot.noteLength = engine::NoteLength::sixteenth;
    slot.seed = 123;
    slot.numEvents = 1;
    slot.events[0] = { 0.0, 1.0 / 16.0, 0, 1.0f, 0.0f, 50, 1, false };

    const auto samplesPerBar = 96000.0;

    const auto run = [&] (int probability, int blockSize)
    {
        slot.events[0].probability = probability;
        engine::PatternScheduler sched;
        sched.prepare (rate);
        engine::PatternScheduler::HostView host;
        host.hasPosition = host.playing = host.hasPpq = host.hasBpm = true;
        host.bpm = 120.0;

        std::vector<int> firedCycles;
        juce::int64 pos = 0;
        const auto total = static_cast<juce::int64> (samplesPerBar * 64.0);
        while (pos < total)
        {
            const auto n = static_cast<int> (juce::jmin<juce::int64> (blockSize, total - pos));
            host.ppq = static_cast<double> (pos) / rate * 2.0;
            engine::PatternScheduler::Trigger trig[engine::PatternScheduler::maxTriggers];
            bool release = false;
            const auto count = sched.process (&snap, host, 0.0f, 120.0, n, 0, true,
                                              trig, release);
            for (int i = 0; i < count; ++i)
                firedCycles.push_back (static_cast<int> (
                    (pos + trig[i].offset) / static_cast<juce::int64> (samplesPerBar)));
            pos += n;
        }
        return firedCycles;
    };

    REQUIRE (run (0, 256).empty());
    REQUIRE (run (100, 256).size() == 64);

    const auto a = run (50, 256);
    const auto b = run (50, 1024); // different block size, same timeline
    const auto c = run (50, 256);  // fresh scheduler, same everything
    REQUIRE (! a.empty());
    REQUIRE (a.size() < 64);       // actually probabilistic
    REQUIRE (a == b);              // block-size independent (bounce == bounce)
    REQUIRE (a == c);              // repeatable

    slot.seed = 124; // a different seed fires a different set
    const auto d = run (50, 256);
    REQUIRE (a != d);
}

// ---- Beat Repeat AUTO chance determinism (Phase 4 open question, fixed here) -----------

TEST_CASE ("Beat Repeat AUTO chance is position-hashed: two passes are identical",
           "[phase5][beatrepeat][determinism]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto render = [] ()
    {
        engine::BeatRepeat br;
        br.prepare (rate, 512);
        br.setRandomSeed (7);
        engine::BeatRepeat::Params p;
        p.mode = 0; // Auto
        p.chancePct = 37.0f;
        p.intervalIndex = 0; // 1/4 bar
        p.gateIndex = 1;
        p.decayPct = 40.0f;

        juce::AudioBuffer<float> out (2, 48000 * 4);
        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            const auto v = std::sin (0.01 * i) * 0.5;
            out.setSample (0, i, static_cast<float> (v));
            out.setSample (1, i, static_cast<float> (v));
        }

        const auto samplesPerBar = 96000.0;
        for (int pos = 0; pos < out.getNumSamples(); pos += 512)
        {
            juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, 512);
            br.process (view, p, static_cast<double> (pos) / samplesPerBar, samplesPerBar);
        }
        return out;
    };

    const auto a = render();
    const auto b = render();
    bool repeated = false;
    for (int i = 0; i < a.getNumSamples(); ++i)
    {
        REQUIRE (juce::exactlyEqual (a.getSample (0, i), b.getSample (0, i)));
        // the stage audibly engaged at least once (output differs from dry)
        repeated = repeated
            || std::abs (a.getSample (0, i)
                         - static_cast<float> (std::sin (0.01 * i) * 0.5)) > 1.0e-3f;
    }
    REQUIRE (repeated);
}

// ---- generators (spec §5) ---------------------------------------------------------------

TEST_CASE ("Generators: seeded randomize repeats, shift/reverse/copy round-trip, undoable",
           "[phase5][generators]")
{
    state::PatternModel m;
    for (int i = 0; i < 8; ++i)
        REQUIRE (m.addEvent (0, i / 16.0, 1.0 / 16.0, i % 4, 100 + i) >= 0);

    const auto snapshotOf = [&m] (int slot)
    {
        std::vector<state::PatternModel::EventView> evs;
        for (int i = 0; i < m.getNumEvents (slot); ++i)
            evs.push_back (m.getEvent (slot, i));
        return evs;
    };
    const auto equalViews = [] (const auto& a, const auto& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (! juce::exactlyEqual (a[i].startBar, b[i].startBar)
                || ! juce::exactlyEqual (a[i].lenBars, b[i].lenBars)
                || a[i].chop != b[i].chop || a[i].velocity != b[i].velocity
                || ! juce::exactlyEqual (a[i].pitch, b[i].pitch)
                || a[i].probability != b[i].probability || a[i].ratchet != b[i].ratchet
                || a[i].reverseFlip != b[i].reverseFlip)
                return false;
        return true;
    };

    const auto original = snapshotOf (0);

    // Same seed + amount on the same pattern = the same result (via undo).
    m.setSeed (0, 42);
    m.setRandomizeAmount (0, 0.75f);
    m.randomize (0, 8);
    const auto first = snapshotOf (0);
    REQUIRE (! equalViews (first, original)); // it changed something
    m.getUndoManager().undo();                // randomize is ONE transaction
    REQUIRE (equalViews (snapshotOf (0), original));
    m.randomize (0, 8);
    REQUIRE (equalViews (snapshotOf (0), first)); // repeatable

    // A different seed gives a different result.
    m.getUndoManager().undo();
    m.setSeed (0, 43);
    m.randomize (0, 8);
    REQUIRE (! equalViews (snapshotOf (0), first));
    m.getUndoManager().undo();
    REQUIRE (equalViews (snapshotOf (0), original));

    // Shift right then left round-trips (and each is undoable).
    m.shiftPattern (0, 1);
    REQUIRE (! equalViews (snapshotOf (0), original));
    m.shiftPattern (0, -1);
    REQUIRE (equalViews (snapshotOf (0), original));

    // Reverse twice round-trips.
    m.reversePattern (0);
    REQUIRE (! equalViews (snapshotOf (0), original));
    m.reversePattern (0);
    REQUIRE (equalViews (snapshotOf (0), original));

    // Copy / paste between slots.
    m.copyToClipboard (0);
    REQUIRE (m.hasClipboard());
    m.pasteFromClipboard (2);
    REQUIRE (equalViews (snapshotOf (2), original));
    m.copyPatternToSlot (0, 5);
    REQUIRE (equalViews (snapshotOf (5), original));
    m.getUndoManager().undo(); // copy-to-slot is undoable
    REQUIRE (m.getNumEvents (5) == 0);
}

// ---- pattern slots (spec §5): bar-quantised switching ----------------------------------

TEST_CASE ("Slot switch quantises to the next bar start while running, immediate when stopped",
           "[phase5][slots]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    eng.prepare (rate, 512, 2);
    REQUIRE (eng.loadBufferSync (makeStaircase (8, 2.0, rate), rate).isEmpty());
    eng.setZeroCrossSnap (false);
    eng.setSliceMode (engine::SliceMode::equal);
    eng.setSensitivity (6.0f / 62.0f);
    REQUIRE (eng.getNumChops() == 8);
    disableChoke (eng);

    auto& pm = eng.getPatternModel();
    pm.addEvent (0, 0.0, 0.25, 0, 127); // slot A: chop 0 (staircase value 0.1)
    pm.addEvent (1, 0.0, 0.25, 2, 127); // slot B: chop 2 (staircase value 0.3)

    SimHost host; // 120 BPM: one bar = 96000 samples
    juce::AudioBuffer<float> full (2, 96000 * 2);
    full.clear();
    juce::AudioBuffer<float> block (2, 512);
    const juce::MidiBuffer noMidi;

    for (int pos = 0; pos < full.getNumSamples(); pos += 512)
    {
        auto ctx = host.ctx();
        // Ask for slot B a quarter of the way into bar 0: the switch must wait
        // for bar 1.
        ctx.patternSlot = pos >= 24000 ? 1 : 0;
        block.clear();
        eng.process (block, noMidi, ctx);
        host.advance (512);
        for (int ch = 0; ch < 2; ++ch)
            full.copyFrom (ch, pos, block, ch, 0, 512);
    }

    // Bar 0 played slot A's chop 0 (0.1), bar 1 slot B's chop 2 (0.3).
    REQUIRE (std::abs (full.getSample (0, 400) - 0.1f) < 0.02f);
    REQUIRE (std::abs (full.getSample (0, 96000 + 400) - 0.3f) < 0.02f);

    // Stopped: the switch is immediate (next run starts on the new slot).
    engine::Engine eng2;
    eng2.prepare (rate, 512, 2);
    REQUIRE (eng2.loadBufferSync (makeStaircase (8, 2.0, rate), rate).isEmpty());
    eng2.setZeroCrossSnap (false);
    eng2.setSliceMode (engine::SliceMode::equal);
    eng2.setSensitivity (6.0f / 62.0f);
    disableChoke (eng2);
    auto& pm2 = eng2.getPatternModel();
    pm2.addEvent (0, 0.0, 0.25, 0, 127);
    pm2.addEvent (1, 0.0, 0.25, 2, 127);

    SimHost host2;
    host2.playing = false;
    juce::AudioBuffer<float> out (2, 512);
    {
        auto ctx = host2.ctx();
        ctx.patternSlot = 1;
        eng2.process (out, noMidi, ctx); // stopped: applies at once
    }
    host2.playing = true;
    host2.ppq = 0.0;
    {
        auto ctx = host2.ctx();
        ctx.patternSlot = 1;
        out.clear();
        eng2.process (out, noMidi, ctx);
    }
    REQUIRE (std::abs (out.getSample (0, 400) - 0.3f) < 0.02f); // slot B from sample 0
}

// ---- granular / freeze (spec §6) --------------------------------------------------------

TEST_CASE ("Granular and freeze: finite output, event-length fill, declick-clean",
           "[phase5][granular]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Declick / level sanity on a DC source (jumps stay windowed-small).
    {
        engine::Engine eng;
        eng.prepare (rate, 512, 2);
        REQUIRE (eng.loadBufferSync (makeConstant (0.5f, 2.0, rate), rate).isEmpty());
        eng.setZeroCrossSnap (false);
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (0.0f);
        REQUIRE (eng.getNumChops() == 2);
        disableChoke (eng);
        auto s = eng.getChopSettings (0);
        s.warp = engine::WarpMode::granular;
        s.grainJitterMs = 0.0f;
        s.grainPitchJitterCents = 0.0f;
        s.grainSprayMs = 0.0f;
        eng.setChopSettings (0, s);

        auto& pm = eng.getPatternModel();
        pm.addEvent (0, 0.0, 0.5, 0, 127);
        SimHost host;
        const auto out = renderWithHost (eng, host, 96000, 512);
        for (int i = 0; i < out.getNumSamples(); ++i)
            REQUIRE (std::isfinite (out.getSample (0, i)));
        REQUIRE (out.getMagnitude (0, 4000, 30000) > 0.1f); // it actually sounds
        REQUIRE (maxSampleJump (out, 0, 1, 96000) < 0.05f); // no clicks
    }

    // Freeze fills the EVENT length and still releases (gate + release).
    {
        engine::Engine eng;
        loadSineChops (eng);
        auto s = eng.getChopSettings (0);
        s.warp = engine::WarpMode::freeze;
        s.mode = engine::PlaybackMode::gate;
        s.releaseMs = 80.0f;
        eng.setChopSettings (0, s);

        auto& pm = eng.getPatternModel();
        pm.addEvent (0, 0.0, 0.5, 0, 127); // span = 1 s at 120 BPM
        SimHost host;
        const auto out = renderWithHost (eng, host, 96000, 512);
        for (int i = 0; i < out.getNumSamples(); ++i)
            REQUIRE (std::isfinite (out.getSample (0, i)));
        // Sustained across the whole event (well past the chop's 100 ms grain).
        REQUIRE (out.getMagnitude (0, 20000, 8000) > 0.05f);
        REQUIRE (out.getMagnitude (0, 40000, 7000) > 0.05f);
        // Gate closed + 80 ms release + margin → silence.
        REQUIRE (out.getMagnitude (0, 48000 + 3840 + 4800, 8000) < 1.0e-3f);
    }

    // One-shot freeze cannot ring forever: it holds one chop's worth.
    {
        engine::Engine eng;
        loadSineChops (eng);
        auto s = eng.getChopSettings (0);
        s.warp = engine::WarpMode::freeze;
        s.mode = engine::PlaybackMode::oneShot;
        eng.setChopSettings (0, s);
        eng.audition (0, 1.0f);
        juce::AudioBuffer<float> block (2, 512);
        renderBlocks (eng, block, 240); // ~2.5 s ≫ the 1 s chop
        REQUIRE (eng.countActiveVoices() == 0);
    }
}

// ---- preset system (spec §10) ------------------------------------------------------------

TEST_CASE ("Presets: category scan, full vs kit-only, mismatched chop mapping",
           "[phase5][presets]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("chopline_preset_tests")
                          .getChildFile (juce::Uuid().toString());
    state::PresetManager manager (root);
    REQUIRE (manager.getPresets().empty());

    // Engine A: 2 s staircase, 4 equal chops, an edited chop + a pattern.
    engine::Engine engA;
    engA.prepare (rate, 512, 2);
    REQUIRE (engA.loadBufferSync (makeStaircase (8, 2.0, rate), rate).isEmpty());
    engA.setZeroCrossSnap (false);
    engA.setSliceMode (engine::SliceMode::equal);
    engA.setSensitivity (2.0f / 62.0f); // → 4 slices
    REQUIRE (engA.getNumChops() == 4);
    {
        auto s = engA.getChopSettings (1);
        s.pitchSemitones = 5;
        s.warp = engine::WarpMode::granular;
        engA.setChopSettings (1, s);
    }
    engA.getPatternModel().addEvent (0, 0.25, 0.25, 1, 90);

    const auto fullState = serializeEngine (engA);

    juce::String error;
    const auto fullFile = manager.save (fullState, "Test Full", "Glitch", false, error);
    REQUIRE (error.isEmpty());
    const auto kitFile = manager.save (fullState, "Test Kit", "Chops", true, error);
    REQUIRE (error.isEmpty());

    // Category scan finds both, sorted by category.
    REQUIRE (manager.getPresets().size() == 2);
    REQUIRE (manager.getPresets()[0].category == "Chops");
    REQUIRE (manager.getPresets()[0].name == "Test Kit");
    REQUIRE (manager.getPresets()[1].category == "Glitch");

    // Kit-only strips the audio + file reference but keeps slices/patterns.
    const auto kitTree = manager.load (kitFile);
    REQUIRE (kitTree.isValid());
    const auto kitSample = kitTree.getChildWithName ("SAMPLE");
    REQUIRE (! kitSample.hasProperty ("flac"));
    REQUIRE (! kitSample.hasProperty ("path"));
    REQUIRE (kitTree.getChildWithName ("SLICES").getNumChildren() == 4);

    // FULL round trip: a fresh engine gets the sample AND the kit.
    {
        engine::Engine engB;
        engB.prepare (rate, 512, 2);
        engB.restoreFrom (manager.load (fullFile), true);
        REQUIRE (engB.currentSample() != nullptr);
        REQUIRE (engB.getNumChops() == 4);
        REQUIRE (engB.getChopSettings (1).pitchSemitones == 5);
        REQUIRE (engB.getChopSettings (1).warp == engine::WarpMode::granular);
        REQUIRE (engB.getPatternModel().getNumEvents (0) == 1);
    }

    // KIT-ONLY onto a DIFFERENT (longer) sample keeps that sample and maps the
    // markers/settings; chop count follows the kit (all markers fit).
    {
        engine::Engine engC;
        engC.prepare (rate, 512, 2);
        REQUIRE (engC.loadBufferSync (makeConstant (0.25f, 3.0, rate), rate).isEmpty());
        const auto before = engC.currentSample();
        engC.restoreFrom (kitTree, true);
        REQUIRE (engC.currentSample() == before); // the sample stayed
        REQUIRE (engC.getNumChops() == 4);
        REQUIRE (engC.getChopSettings (1).pitchSemitones == 5);
        REQUIRE (engC.getPatternModel().getNumEvents (0) == 1);
    }

    // KIT-ONLY onto a SHORTER sample: markers beyond its end drop (documented
    // mapping rule); events pointing past the chop count stay silent.
    {
        engine::Engine engD;
        engD.prepare (rate, 512, 2);
        REQUIRE (engD.loadBufferSync (makeConstant (0.25f, 0.8, rate), rate).isEmpty());
        engD.restoreFrom (kitTree, true);
        // Kit markers at 0 / 0.5 / 1.0 / 1.5 s → only 0 and 0.5 fit into 0.8 s.
        REQUIRE (engD.getNumChops() == 2);
    }

    root.getParentDirectory().deleteRecursively();
}

// ---- drag-out MIDI (spec §10): the file parses back to the pattern ---------------------

TEST_CASE ("Drag-out MIDI matches the pattern (parse-back)", "[phase5][export][midi]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    state::PatternModel m;
    const auto a = m.addEvent (0, 0.0, 0.25, 0, 127);
    const auto b = m.addEvent (0, 0.5, 0.25, 3, 64);
    REQUIRE (a >= 0);
    REQUIRE (b >= 0);
    m.setEventRatchet (0, b, 4); // exports as its audible subdivision

    const auto midi = state::buildPatternMidi (m, 0, 36, 128.0);

    juce::MemoryOutputStream out;
    REQUIRE (midi.writeTo (out, 1));
    juce::MemoryInputStream in (out.getData(), out.getDataSize(), false);
    juce::MidiFile parsed;
    REQUIRE (parsed.readFrom (in));
    REQUIRE (parsed.getNumTracks() == 1);

    const auto* track = parsed.getTrack (0);
    std::vector<const juce::MidiMessage*> ons;
    double tempoSecPerQuarter = 0.0;
    for (int i = 0; i < track->getNumEvents(); ++i)
    {
        const auto& msg = track->getEventPointer (i)->message;
        if (msg.isNoteOn())
            ons.push_back (&msg);
        else if (msg.isTempoMetaEvent())
            tempoSecPerQuarter = msg.getTempoSecondsPerQuarterNote();
    }

    REQUIRE (ons.size() == 5); // 1 plain + 4 ratchet hits
    REQUIRE (tempoSecPerQuarter == Approx (60.0 / 128.0).margin (1.0e-4));

    // Event A: chop 0 → note 36 at tick 0, full velocity.
    REQUIRE (ons[0]->getNoteNumber() == 36);
    REQUIRE (ons[0]->getTimeStamp() == Approx (0.0));
    REQUIRE (ons[0]->getVelocity() == 127);

    // Event B: chop 3 → note 39, 4 ratchets from bar 0.5 (= 2 quarters = 1920
    // ticks), spaced 1/16 bar (= 240 ticks), velocities decaying by 0.8×.
    for (int k = 0; k < 4; ++k)
    {
        const auto& on = *ons[static_cast<size_t> (1 + k)];
        REQUIRE (on.getNoteNumber() == 39);
        REQUIRE (on.getTimeStamp() == Approx (1920.0 + 240.0 * k).margin (1.0));
    }
    REQUIRE (ons[1]->getVelocity() == 64);
    REQUIRE (static_cast<float> (ons[2]->getVelocity())
             == Approx (64.0f * 0.8f).margin (1.0f));
}

// ---- offline render (spec §10/§8): deterministic, bounce == bounce ---------------------

TEST_CASE ("Offline pattern render is deterministic (incl. prob, ratchet, granular, BR auto)",
           "[phase5][export][render]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    engine::Engine eng;
    eng.prepare (rate, 512, 2);
    REQUIRE (eng.loadBufferSync (makeSine (330.0, 2.0, rate), rate).isEmpty());
    eng.setZeroCrossSnap (false);
    eng.setSliceMode (engine::SliceMode::equal);
    eng.setSensitivity (2.0f / 62.0f); // 4 chops
    {
        auto s = eng.getChopSettings (2);
        s.warp = engine::WarpMode::granular;
        eng.setChopSettings (2, s);
    }
    auto& pm = eng.getPatternModel();
    const auto e1 = pm.addEvent (0, 0.0, 0.25, 0, 120);
    const auto e2 = pm.addEvent (0, 0.25, 0.25, 2, 110);
    const auto e3 = pm.addEvent (0, 0.5, 0.25, 1, 100);
    pm.setEventRatchet (0, e1, 3);
    pm.setEventProbability (0, e2, 50);
    pm.setEventPitch (0, e3, -5.0f);

    const auto fullState = serializeEngine (eng);

    state::OfflineRenderSpec spec;
    spec.sampleRate = rate;
    spec.bpm = 124.0;
    spec.context.br.mode = 0; // BR Auto engaged in the render
    spec.context.br.chancePct = 60.0f;
    spec.context.master.filterOn = true;
    spec.context.master.cutoffHz = 9000.0f;
    spec.context.master.outOn = true;

    juce::String err1, err2;
    const auto a = state::renderPatternOffline (fullState, spec, err1);
    const auto b = state::renderPatternOffline (fullState, spec, err2);
    REQUIRE (err1.isEmpty());
    REQUIRE (err2.isEmpty());
    REQUIRE (a.getNumSamples() > 0);
    REQUIRE (a.getNumSamples() == b.getNumSamples());
    REQUIRE (a.getMagnitude (0, 0, a.getNumSamples()) > 0.01f);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            REQUIRE (std::isfinite (a.getSample (ch, i)));
            REQUIRE (juce::exactlyEqual (a.getSample (ch, i), b.getSample (ch, i)));
        }
}

// ---- Eco vs HQ (spec §8) ------------------------------------------------------------------

TEST_CASE ("Eco and HQ both render clean (stretch + granular + master)", "[phase5][quality]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    for (const bool eco : { false, true })
    {
        INFO ((eco ? "Eco" : "HQ"));
        engine::Engine eng;
        eng.setQualityEco (eco);
        loadSineChops (eng);
        {
            auto s = eng.getChopSettings (0);
            s.warp = engine::WarpMode::stretch;
            s.pitchSemitones = 4;
            eng.setChopSettings (0, s);
            auto s2 = eng.getChopSettings (1);
            s2.warp = engine::WarpMode::granular;
            s2.grainDensity = 6; // Eco caps this at 2 per trigger
            eng.setChopSettings (1, s2);
        }
        auto& pm = eng.getPatternModel();
        pm.addEvent (0, 0.0, 0.25, 0, 127);
        pm.addEvent (0, 0.5, 0.25, 1, 127);

        SimHost host;
        juce::AudioBuffer<float> full (2, 96000);
        full.clear();
        juce::AudioBuffer<float> block (2, 512);
        const juce::MidiBuffer noMidi;
        for (int pos = 0; pos < full.getNumSamples(); pos += 512)
        {
            auto ctx = host.ctx();
            ctx.eco = eco;
            ctx.master.distOn = true;
            ctx.master.distType = 2; // Tape: base rate in Eco
            ctx.master.drivePct = 40.0f;
            ctx.master.ecoNoOversample = eco;
            ctx.master.outOn = true;
            block.clear();
            eng.process (block, noMidi, ctx);
            host.advance (512);
            for (int ch = 0; ch < 2; ++ch)
                full.copyFrom (ch, pos, block, ch, 0, 512);
        }

        for (int i = 0; i < full.getNumSamples(); ++i)
            REQUIRE (std::isfinite (full.getSample (0, i)));
        REQUIRE (full.getMagnitude (0, 2000, 20000) > 0.05f);
        REQUIRE (full.getMagnitude (0, 48000 + 2000, 20000) > 0.02f);

        // Toggling quality at runtime re-preps cleanly too.
        eng.setQualityEco (! eco);
        eng.audition (0, 1.0f);
        renderBlocks (eng, block, 20);
        REQUIRE (std::isfinite (block.getSample (0, 0)));
    }
}

// ---- state compatibility: a Phase 4-shaped state (no Phase 5 keys) loads ----------------

TEST_CASE ("Phase 4-shaped state (no seed/rev/granular keys) loads with defaults",
           "[phase5][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    engine::Engine eng;
    eng.prepare (rate, 512, 2);
    REQUIRE (eng.loadBufferSync (makeStaircase (4, 1.0, rate), rate).isEmpty());
    eng.setZeroCrossSnap (false);
    eng.setSliceMode (engine::SliceMode::equal);
    eng.setSensitivity (2.0f / 62.0f);
    auto tree = serializeEngine (eng);

    // Strip every Phase 5 key, as a Phase 4 build would have written it.
    auto slices = tree.getChildWithName ("SLICES");
    for (auto marker : slices)
        for (const auto* key : { "gs", "gd", "gj", "gp", "gy" })
            marker.removeProperty (key, nullptr);
    auto patterns = tree.getChildWithName ("PATTERNS");
    for (auto p : patterns)
    {
        p.removeProperty ("seed", nullptr);
        p.removeProperty ("rndAmt", nullptr);
    }

    engine::Engine engB;
    engB.prepare (rate, 512, 2);
    engB.restoreFrom (tree, true);
    REQUIRE (engB.getNumChops() == 4);
    const engine::ChopSettings defaults;
    const auto s = engB.getChopSettings (0);
    REQUIRE (juce::exactlyEqual (s.grainMs, defaults.grainMs));
    REQUIRE (s.grainDensity == defaults.grainDensity);
    REQUIRE (engB.getPatternModel().getSeed (0) == 1);
}

// Hidden CPU benchmark (not run by default / CI): Release build, 8 stretch
// voices + ratchets + Beat Repeat + full master at 48 kHz / 256 (spec §8
// target: < 5 % of one M1 core). Run with "[.bench]" and read the INFO line.
TEST_CASE ("CPU: 8 stretch voices + BR + master at 48k/256", "[.bench]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    for (const bool eco : { false, true })
    {
        engine::Engine eng;
        eng.setQualityEco (eco);
        eng.prepare (rate, 256, 2);
        REQUIRE (eng.loadBufferSync (makeSine (330.0, 2.0, rate), rate).isEmpty());
        eng.setZeroCrossSnap (false);
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (6.0f / 62.0f); // 8 chops
        disableChoke (eng);
        for (int i = 0; i < 8; ++i)
        {
            auto s = eng.getChopSettings (i);
            s.warp = engine::WarpMode::stretch;
            s.pitchSemitones = (i % 5) - 2;
            s.mode = engine::PlaybackMode::gate;
            eng.setChopSettings (i, s);
        }
        auto& pm = eng.getPatternModel();
        for (int i = 0; i < 8; ++i)
            pm.addEvent (0, i / 8.0, 1.0 / 8.0, i, 120); // 8 overlapping-release voices

        SimHost host;
        juce::AudioBuffer<float> block (2, 256);
        const juce::MidiBuffer noMidi;
        const auto fill = [] (engine::Engine::BlockContext ctx, bool ecoFlag)
        {
            ctx.br.mode = 0;
            ctx.br.chancePct = 60.0f;
            ctx.master.filterOn = true;
            ctx.master.cutoffHz = 12000.0f;
            ctx.master.distOn = true;
            ctx.master.drivePct = 30.0f;
            ctx.master.widthOn = true;
            ctx.master.outOn = true;
            ctx.master.delayOn = true;
            ctx.master.delayRtnPct = 30.0f;
            ctx.master.verbOn = true;
            ctx.master.verbRtnPct = 30.0f;
            ctx.eco = ecoFlag;
            ctx.master.ecoNoOversample = ecoFlag;
            return ctx;
        };

        // Warm-up.
        for (int b = 0; b < 400; ++b)
        {
            eng.process (block, noMidi, fill (host.ctx(), eco));
            host.advance (256);
        }

        constexpr int numBlocks = 4000; // ~21 s of audio
        const auto t0 = juce::Time::getHighResolutionTicks();
        for (int b = 0; b < numBlocks; ++b)
        {
            eng.process (block, noMidi, fill (host.ctx(), eco));
            host.advance (256);
        }
        const auto elapsedSec = juce::Time::highResolutionTicksToSeconds (
            juce::Time::getHighResolutionTicks() - t0);
        const auto audioSec = numBlocks * 256.0 / rate;
        WARN ((eco ? "[bench ECO] " : "[bench HQ ] ")
              << juce::String (100.0 * elapsedSec / audioSec, 2) << "% of one core "
              << "(" << juce::String (elapsedSec, 3) << " s compute for "
              << juce::String (audioSec, 1) << " s of audio)");
        REQUIRE (elapsedSec < audioSec); // sanity: comfortably faster than real time
    }
}
