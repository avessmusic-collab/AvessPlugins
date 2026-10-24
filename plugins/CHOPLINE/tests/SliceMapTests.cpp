// CHOPLINE tests — slicing modes, determinism, zero-cross snap, manual edits.

#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"

using namespace chopline;
using namespace chopline::test;

namespace
{
    std::shared_ptr<const engine::SampleData> loadSample (engine::SampleStore& store,
                                                          const juce::AudioBuffer<float>& buf,
                                                          double rate)
    {
        REQUIRE (store.loadBufferSync (buf, rate, rate).isEmpty());
        auto data = store.current();
        REQUIRE (data != nullptr);
        return data;
    }
} // namespace

TEST_CASE ("Equal mode: SENS maps to 2–64 slices of equal length", "[slicemap]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::SampleStore store;
    const auto data = loadSample (store, makeConstant (0.5f, 2.0, 48000.0), 48000.0);

    engine::SliceModel model;
    model.mode = engine::SliceMode::equal;
    model.zeroCrossSnap = false; // DC has no crossings
    model.minSliceLengthMs = 20.0f;

    model.sensitivity = 0.0f;
    model.regenerate (data.get());
    REQUIRE (model.getMarkers().size() == 2);

    model.sensitivity = 1.0f;
    model.regenerate (data.get());
    REQUIRE (model.getMarkers().size() == 64);

    const auto snap = model.buildSnapshot (data.get());
    REQUIRE (snap.numChops == 64);
    const auto expectedLen = data->numFrames() / 64;
    for (int i = 0; i < snap.numChops; ++i)
    {
        const auto len = snap.chops[static_cast<size_t> (i)].end
                       - snap.chops[static_cast<size_t> (i)].start;
        REQUIRE (std::llabs (len - expectedLen) <= 2);
    }
}

TEST_CASE ("Grid mode: markers at BPM divisions chosen by SENS", "[slicemap]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::SampleStore store;
    const auto data = loadSample (store, makeConstant (0.5f, 4.0, 48000.0), 48000.0);

    engine::SliceModel model;
    model.mode = engine::SliceMode::grid;
    model.zeroCrossSnap = false;
    model.sampleBpm = 120.0;       // quarter = 0.5 s
    model.sensitivity = 0.6f;      // division index 2 → 1/16 → 0.125 s
    model.minSliceLengthMs = 20.0f;
    model.regenerate (data.get());

    const auto& markers = model.getMarkers();
    REQUIRE (markers.size() >= 2);
    REQUIRE (juce::exactlyEqual (markers[0].timeSec, 0.0));
    REQUIRE (std::abs (markers[1].timeSec - 0.125) < 1.0e-9);
    // 4 s / 0.125 s = 32 steps, capped at 32 markers (< 64).
    REQUIRE (markers.size() == 32);
}

TEST_CASE ("Slicing is deterministic: same input + sensitivity → identical markers",
           "[slicemap]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto audio = makeClickTrain ({ 0.3, 0.8, 1.3, 1.9 }, 2.5, 48000.0);

    auto run = [&audio]
    {
        engine::SampleStore store;
        auto data = store.current();
        REQUIRE (store.loadBufferSync (audio, 48000.0, 48000.0).isEmpty());
        data = store.current();

        engine::SliceModel model;
        model.mode = engine::SliceMode::transient;
        model.sensitivity = 0.55f;
        model.regenerate (data.get());

        std::vector<double> times;
        for (const auto& m : model.getMarkers())
            times.push_back (m.timeSec);
        return times;
    };

    const auto a = run();
    const auto b = run();
    REQUIRE (a == b);
    REQUIRE (a.size() >= 4);
}

TEST_CASE ("Zero-cross snap moves a marker to the nearest crossing", "[slicemap]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::SampleStore store;
    constexpr double rate = 48000.0;
    // 100 Hz sine: crossings every 5 ms exactly.
    const auto data = loadSample (store, makeSine (100.0, 1.0, rate), rate);

    const auto snapped = engine::SliceModel::snapToZeroCross (*data, 0.0523);
    // Nearest crossings to 52.3 ms are 50 ms and 55 ms → expect ≈ 50 ms... the
    // nearest is 50 ms (2.3 ms away) vs 55 ms (2.7 ms) → 50 ms.
    REQUIRE (std::abs (snapped - 0.050) < 0.0005);

    // The snapped position really sits on a (near-)zero sample.
    const auto idx = static_cast<int> (std::llround (snapped * rate));
    REQUIRE (std::abs (data->audio.getSample (0, idx)) < 0.02f);
}

TEST_CASE ("Manual edits: add, clamp-move, delete; marker 0 fixed", "[slicemap]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::SampleStore store;
    const auto data = loadSample (store, makeConstant (0.4f, 2.0, 48000.0), 48000.0);

    engine::SliceModel model;
    model.mode = engine::SliceMode::manual;
    model.zeroCrossSnap = false;
    model.minSliceLengthMs = 100.0f;
    model.regenerate (data.get()); // seeds the single whole-sample chop
    REQUIRE (model.getMarkers().size() == 1);
    REQUIRE (juce::exactlyEqual (model.getMarkers()[0].timeSec, 0.0));

    const auto id1 = model.addMarker (1.0, data.get());
    REQUIRE (id1 >= 0);
    REQUIRE (model.getMarkers().size() == 2);

    // Too close to an existing marker (min 100 ms) → rejected.
    REQUIRE (model.addMarker (1.05, data.get()) == -1);

    // Move clamps between neighbours ± min length.
    REQUIRE (model.moveMarker (id1, 0.01, data.get()));
    REQUIRE (model.getMarkers()[1].timeSec >= 0.1 - 1.0e-9);
    REQUIRE (model.moveMarker (id1, 5.0, data.get()));
    REQUIRE (model.getMarkers()[1].timeSec <= 1.9 + 1.0e-9);

    // The 0 s marker can be neither moved nor removed.
    const auto id0 = model.getMarkers()[0].id;
    REQUIRE (! model.moveMarker (id0, 0.5, data.get()));
    REQUIRE (! model.removeMarker (id0));

    REQUIRE (model.removeMarker (id1));
    REQUIRE (model.getMarkers().size() == 1);
}

TEST_CASE ("Minimum slice length filters transient markers", "[slicemap]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::SampleStore store;
    // Clicks 60 ms apart; min length 200 ms must merge them.
    const auto data = loadSample (store,
                                  makeClickTrain ({ 0.3, 0.36, 0.42, 1.0, 1.06 }, 1.6, 48000.0),
                                  48000.0);

    engine::SliceModel model;
    model.mode = engine::SliceMode::transient;
    model.sensitivity = 0.9f;
    model.minSliceLengthMs = 200.0f;
    model.regenerate (data.get());

    const auto& markers = model.getMarkers();
    for (size_t i = 1; i < markers.size(); ++i)
        REQUIRE (markers[i].timeSec - markers[i - 1].timeSec >= 0.2 - 1.0e-6);
}

TEST_CASE ("State restore round trip keeps marker times and fades", "[slicemap][state]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    engine::Engine eng;
    eng.prepare (48000.0, 512, 2);
    REQUIRE (eng.loadBufferSync (makeClickTrain ({ 0.4, 0.9, 1.4 }, 2.0, 48000.0), 48000.0).isEmpty());
    REQUIRE (eng.getNumChops() >= 2);

    // Nudge a marker so the restored map is provably not a re-slice.
    const auto movedId = eng.getMarkers()[1].id;
    REQUIRE (eng.moveMarker (movedId, eng.getMarkers()[1].timeSec + 0.05));
    std::vector<double> expectedTimes;
    for (const auto& m : eng.getMarkers())
        expectedTimes.push_back (m.timeSec);

    juce::ValueTree root ("CHOPLINE");
    eng.serializeTo (root);
    REQUIRE (root.getChildWithName (engine::Engine::sampleTag).isValid());
    REQUIRE (root.getChildWithName (engine::Engine::slicesTag).isValid());

    engine::Engine restored;
    restored.prepare (48000.0, 512, 2);
    restored.restoreFrom (root, true); // synchronous for the test

    REQUIRE (restored.currentSample() != nullptr);
    const auto& markers = restored.getMarkers();
    REQUIRE (markers.size() == expectedTimes.size());
    for (size_t i = 0; i < markers.size(); ++i)
        REQUIRE (std::abs (markers[i].timeSec - expectedTimes[i]) < 1.0e-6);
}
