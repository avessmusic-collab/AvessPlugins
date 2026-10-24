// CHOPLINE tests — voice pool + declick (spec §12): no sample-to-sample jump
// above threshold at chop boundaries; 17th trigger steals the oldest voice
// without a crash and the stolen voice fades.

#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"
#include "engine/VoicePool.h"

using namespace chopline;
using namespace chopline::test;

namespace
{
    constexpr double rate = 48000.0;

    // 2 ms equal-power fade at 48 kHz = 96 steps; max per-sample step of the
    // sine ramp ≈ sin(π/2 · 1/96) ≈ 0.016. Give slack for summed edges.
    constexpr float declickThreshold = 0.06f;

    struct Rig
    {
        engine::SampleStore store;
        std::shared_ptr<const engine::SampleData> data;
        engine::SliceMapSnapshot slices;

        explicit Rig (int numChops, double lengthSec = 2.0)
        {
            // DC buffer makes fades directly visible in the output.
            REQUIRE (store.loadBufferSync (makeConstant (1.0f, lengthSec, rate), rate, rate).isEmpty());
            data = store.current();
            REQUIRE (data != nullptr);

            engine::SliceModel model;
            model.mode = engine::SliceMode::equal;
            model.zeroCrossSnap = false;
            model.minSliceLengthMs = 20.0f;
            model.sensitivity = numChops >= 2
                ? static_cast<float> (numChops - 2) / 62.0f
                : 0.0f;
            model.regenerate (data.get());
            slices = model.buildSnapshot (data.get());
            REQUIRE (slices.numChops == numChops);
        }
    };
} // namespace

TEST_CASE ("Declick: a triggered chop ramps in and out without jumps", "[voice][declick]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Rig rig (8);

    engine::VoicePool pool;
    pool.prepare (rate);

    // Render one chop fully (chop 3 starts mid-sample at value 1.0 — the declick
    // fade is the only thing standing between that and a click).
    const auto& chop = rig.slices.chops[3];
    const auto chopLen = static_cast<int> (chop.end - chop.start);

    juce::AudioBuffer<float> out (2, chopLen + 4800);
    out.clear();
    pool.trigger (rig.data.get(), chop, 3, 1.0f);
    pool.renderAdd (out, 0, out.getNumSamples());

    // Starts from (near) zero, reaches full level, ends back at zero.
    REQUIRE (std::abs (out.getSample (0, 0)) < declickThreshold);
    REQUIRE (out.getMagnitude (0, 0, out.getNumSamples()) > 0.98f);
    REQUIRE (std::abs (out.getSample (0, out.getNumSamples() - 1)) < 1.0e-6f);

    // No jump above threshold anywhere (both channels).
    REQUIRE (maxSampleJump (out, 0) < declickThreshold);
    REQUIRE (maxSampleJump (out, 1) < declickThreshold);

    REQUIRE (pool.countActive() == 0); // chop finished, voice freed
}

TEST_CASE ("Sliced playback of every chop is click-free at the boundaries",
           "[voice][declick]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Rig rig (16);

    engine::VoicePool pool;
    pool.prepare (rate);

    // Play chops back to back (each trigger exactly at the previous boundary).
    juce::AudioBuffer<float> out (2, static_cast<int> (rig.data->numFrames()) + 4800);
    out.clear();

    int cursor = 0;
    for (int c = 0; c < rig.slices.numChops; ++c)
    {
        const auto start = static_cast<int> (rig.slices.chops[static_cast<size_t> (c)].start);
        pool.renderAdd (out, cursor, start - cursor);
        cursor = start;
        pool.trigger (rig.data.get(), rig.slices.chops[static_cast<size_t> (c)], c, 1.0f);
    }
    pool.renderAdd (out, cursor, out.getNumSamples() - cursor);

    REQUIRE (maxSampleJump (out, 0) < 2.0f * declickThreshold); // fade-out overlaps fade-in
}

TEST_CASE ("17th trigger steals the oldest voice; stolen audio fades, no crash",
           "[voice][steal]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Rig rig (24);

    engine::VoicePool pool;
    pool.prepare (rate);

    // Fill all 16 voices.
    for (int c = 0; c < 16; ++c)
        pool.trigger (rig.data.get(), rig.slices.chops[static_cast<size_t> (c)], c, 1.0f);
    REQUIRE (pool.countActive() == 16);

    // Let them run a little so everyone is past the fade-in at full level.
    juce::AudioBuffer<float> out (2, 512);
    out.clear();
    pool.renderAdd (out, 0, 512);

    // 17th trigger → steal. Still 16 voices, no crash, output continuous.
    out.clear();
    pool.trigger (rig.data.get(), rig.slices.chops[16], 16, 1.0f);
    REQUIRE (pool.countActive() == 16);

    pool.renderAdd (out, 0, 512);
    // 15 sustained voices at 1.0 plus a crossfade (2 ms tail out, new voice in):
    // any discontinuity above the declick threshold means the steal clicked.
    REQUIRE (maxSampleJump (out, 0, 1) < 2.0f * declickThreshold);

    // The stolen voice's old stream dies within ~2 ms: after 4 ms the summed
    // level settles back to 16 full-level voices.
    juce::AudioBuffer<float> later (2, 512);
    later.clear();
    pool.renderAdd (later, 0, 512);
    REQUIRE (std::abs (later.getSample (0, 256) - 16.0f) < 0.2f);
}

TEST_CASE ("fastRelease (all-notes-off) fades everything out", "[voice]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Rig rig (4);

    engine::VoicePool pool;
    pool.prepare (rate);
    pool.trigger (rig.data.get(), rig.slices.chops[0], 0, 1.0f);
    pool.trigger (rig.data.get(), rig.slices.chops[1], 1, 1.0f);

    juce::AudioBuffer<float> out (2, 1024);
    out.clear();
    pool.renderAdd (out, 0, 1024); // past the fade-in

    pool.releaseAll();
    out.clear();
    pool.renderAdd (out, 0, 1024); // 2 ms fade = 96 samples

    REQUIRE (maxSampleJump (out, 0) < 2.0f * declickThreshold);
    REQUIRE (pool.countActive() == 0);
    REQUIRE (std::abs (out.getSample (0, 512)) < 1.0e-6f);
}
