// CHOPLINE tests — Engine: MIDI note→chop mapping (sample-accurate), the
// audition FIFO path, and the real-time allocation discipline (spec §8/§12:
// a debug allocation detector armed around processBlock-equivalent calls).

#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"

#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

using namespace chopline;
using namespace chopline::test;

// ---- global allocation detector -------------------------------------------------
// Replaces global new/delete for this TEST BINARY. While `armed` is true and
// the calling thread is the armed thread, any allocation or deallocation
// counts as a violation (the audio thread must do neither, spec §8).

namespace allocGuard
{
    std::atomic<bool> armed { false };
    std::atomic<int> violations { 0 };
    std::thread::id armedThread; // written only while disarmed

    inline void note() noexcept
    {
        if (armed.load (std::memory_order_relaxed)
            && std::this_thread::get_id() == armedThread)
            violations.fetch_add (1, std::memory_order_relaxed);
    }

    struct Arm
    {
        Arm()
        {
            armedThread = std::this_thread::get_id();
            violations.store (0);
            armed.store (true);
        }
        ~Arm() { armed.store (false); }
    };
} // namespace allocGuard

void* operator new (std::size_t size)
{
    allocGuard::note();
    if (auto* p = std::malloc (size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    allocGuard::note();
    if (auto* p = std::malloc (size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}

void* operator new (std::size_t size, std::align_val_t align)
{
    allocGuard::note();
    void* p = nullptr;
    if (posix_memalign (&p, juce::jmax (sizeof (void*), static_cast<size_t> (align)),
                        size > 0 ? size : 1) == 0)
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size, std::align_val_t align)
{
    return operator new (size, align);
}

void operator delete (void* p) noexcept { allocGuard::note(); std::free (p); }
void operator delete[] (void* p) noexcept { allocGuard::note(); std::free (p); }
void operator delete (void* p, std::size_t) noexcept { allocGuard::note(); std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { allocGuard::note(); std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept { allocGuard::note(); std::free (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { allocGuard::note(); std::free (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { allocGuard::note(); std::free (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { allocGuard::note(); std::free (p); }

// ---- tests -------------------------------------------------------------------------

namespace
{
    constexpr double rate = 48000.0;

    void loadEightChops (engine::Engine& eng)
    {
        eng.prepare (rate, 512, 2);
        REQUIRE (eng.loadBufferSync (makeStaircase (8, 2.0, rate), rate).isEmpty());
        eng.setZeroCrossSnap (false);           // staircase has no crossings
        eng.setSliceMode (engine::SliceMode::equal);
        eng.setSensitivity (6.0f / 62.0f);      // → exactly 8 equal slices
        REQUIRE (eng.getNumChops() == 8);
    }
} // namespace

TEST_CASE ("MIDI note → chop mapping, sample-accurate offset and velocity",
           "[engine][midi]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadEightChops (eng);

    juce::AudioBuffer<float> out (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 36 + 3, static_cast<juce::uint8> (127)), 100);

    const engine::Engine::BlockContext ctx { 36 }; // base C1
    eng.process (out, midi, ctx);

    // Silence before the event's sample position.
    REQUIRE (juce::exactlyEqual (out.getMagnitude (0, 0, 100), 0.0f));
    // Sound exactly from the event (fade-in starts at sample 100).
    REQUIRE (std::abs (out.getSample (0, 100)) > 0.0f);
    // After the 2 ms declick fade: chop 3 of the staircase = 0.4, velocity 1.
    REQUIRE (std::abs (out.getSample (0, 400) - 0.4f) < 0.01f);
    // Velocity scales the gain on a second note (chop 5 = 0.6, velocity 64/127).
    juce::MidiBuffer midi2;
    midi2.addEvent (juce::MidiMessage::noteOn (1, 36 + 5, static_cast<juce::uint8> (64)), 0);
    juce::AudioBuffer<float> out2 (2, 512);
    eng.process (out2, midi2, ctx);
    const auto expected = 0.6f * juce::MidiMessage::noteOn (1, 41, static_cast<juce::uint8> (64))
                                     .getFloatVelocity();
    // (out2 still carries the tail of the first chop? No — chop 3 plays on, so
    //  subtract its contribution: it sits at 0.4.)
    REQUIRE (std::abs (out2.getSample (0, 400) - (0.4f + expected)) < 0.02f);

    // Notes below the base or beyond the chop count do nothing (no crash).
    juce::MidiBuffer midi3;
    midi3.addEvent (juce::MidiMessage::noteOn (1, 20, static_cast<juce::uint8> (100)), 0);
    midi3.addEvent (juce::MidiMessage::noteOn (1, 36 + 60, static_cast<juce::uint8> (100)), 0);
    juce::AudioBuffer<float> out3 (2, 512);
    eng.process (out3, midi3, ctx);
    const auto before = eng.countActiveVoices();
    REQUIRE (before == 2); // still just the two playing chops
}

TEST_CASE ("Configurable base note shifts the mapping", "[engine][midi]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadEightChops (eng);

    juce::AudioBuffer<float> out (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 48 + 2, static_cast<juce::uint8> (127)), 0);

    eng.process (out, midi, { 48 }); // base C2
    REQUIRE (std::abs (out.getSample (0, 400) - 0.3f) < 0.01f); // chop 2 = 0.3
}

TEST_CASE ("Audition path triggers the same voice path lock-free", "[engine][audition]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadEightChops (eng);

    eng.audition (2, 0.5f);

    juce::AudioBuffer<float> out (2, 512);
    renderBlocks (eng, out, 1);

    // Chop 2 = 0.3, velocity 0.5 → 0.15 after the fade.
    REQUIRE (std::abs (out.getSample (0, 400) - 0.15f) < 0.01f);
    REQUIRE (eng.countActiveVoices() == 1);
}

TEST_CASE ("Audio thread allocates nothing (allocation detector over many blocks)",
           "[engine][realtime]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    loadEightChops (eng);

    juce::AudioBuffer<float> out (2, 512);

    // Prebuild MIDI buffers outside the armed region.
    juce::MidiBuffer notes;
    notes.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (120)), 7);
    notes.addEvent (juce::MidiMessage::noteOn (1, 43, static_cast<juce::uint8> (90)), 250);
    juce::MidiBuffer empty;
    const engine::Engine::BlockContext ctx { 36 };

    // Warm-up (first calls may touch lazily-initialised library state).
    eng.audition (1, 0.8f);
    eng.process (out, notes, ctx);
    eng.process (out, empty, ctx);

    // ~10 s of audio at 512-sample blocks with steady triggering (spec §12).
    constexpr int numBlocks = 1000;
    {
        allocGuard::Arm arm;
        for (int b = 0; b < numBlocks; ++b)
            eng.process (out, (b % 4 == 0) ? notes : empty, ctx);
    }

    REQUIRE (allocGuard::violations.load() == 0);
}

// Hidden manual smoke test (not run by default or in CI): set
//   CHOPLINE_SMOKE_FILE=<path to a real vocal>   (dev/test only, never embedded)
//   CHOPLINE_SMOKE_OUT=<dir for the rendered WAV>  (optional)
// and run the binary with "[.smoke]". Verifies the real-file load → transient
// slice → MIDI trigger → render path and writes an audition render to listen to.
TEST_CASE ("Real vocal: load, slice, trigger, offline render", "[.smoke]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto path = juce::SystemStats::getEnvironmentVariable ("CHOPLINE_SMOKE_FILE", {});
    if (path.isEmpty())
    {
        SUCCEED ("CHOPLINE_SMOKE_FILE not set — skipping");
        return;
    }

    engine::Engine eng;
    eng.prepare (rate, 512, 2);
    const auto error = eng.loadFileSync (juce::File (path));
    INFO ("load error: " << error);
    REQUIRE (error.isEmpty());

    const auto data = eng.currentSample();
    REQUIRE (data != nullptr);
    INFO ("length " << data->lengthSeconds() << " s, detected BPM " << data->detectedBpm);

    // Default transient slicing must produce a useful number of chops.
    const auto numChops = eng.getNumChops();
    INFO ("chops at default SENS: " << numChops);
    REQUIRE (numChops >= 2);

    // Trigger the first chops from MIDI, half a second apart, render to a WAV.
    const engine::Engine::BlockContext ctx { 36 };
    const auto chopsToPlay = juce::jmin (8, numChops);
    const auto renderLen = static_cast<int> (rate * (0.5 * chopsToPlay + 1.0));
    juce::AudioBuffer<float> out (2, 512);
    juce::AudioBuffer<float> full (2, renderLen);
    full.clear();

    float peak = 0.0f;
    for (int pos = 0; pos + 512 <= renderLen; pos += 512)
    {
        juce::MidiBuffer midi;
        const auto beat = pos / static_cast<int> (rate * 0.5);
        const auto beatStart = beat * static_cast<int> (rate * 0.5);
        if (beat < chopsToPlay && beatStart >= pos && beatStart < pos + 512)
            midi.addEvent (juce::MidiMessage::noteOn (1, 36 + beat, static_cast<juce::uint8> (110)),
                           beatStart - pos);
        eng.process (out, midi, ctx);
        for (int ch = 0; ch < 2; ++ch)
            full.copyFrom (ch, pos, out, ch, 0, 512);
        peak = juce::jmax (peak, out.getMagnitude (0, 0, 512));
        for (int i = 0; i < 512; ++i)
            REQUIRE (std::isfinite (out.getSample (0, i)));
    }
    REQUIRE (peak > 0.05f); // the chops actually sounded

    const auto outDir = juce::SystemStats::getEnvironmentVariable ("CHOPLINE_SMOKE_OUT", {});
    if (outDir.isNotEmpty())
    {
        const auto file = juce::File (outDir).getChildFile ("chopline_smoke_render.wav");
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        REQUIRE (stream != nullptr);
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (rate)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        REQUIRE (writer->writeFromAudioSampleBuffer (full, 0, full.getNumSamples()));
    }
}

TEST_CASE ("Block sizes from 1 to 4096 render safely (spec §2)", "[engine]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    engine::Engine eng;
    eng.prepare (rate, 4096, 2);
    REQUIRE (eng.loadBufferSync (makeStaircase (8, 1.0, rate), rate).isEmpty());
    eng.setZeroCrossSnap (false);
    eng.setSliceMode (engine::SliceMode::equal);
    eng.setSensitivity (6.0f / 62.0f);

    const engine::Engine::BlockContext ctx { 36 };
    for (const int blockSize : { 1, 3, 16, 333, 512, 4096 })
    {
        juce::AudioBuffer<float> out (2, blockSize);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 36, static_cast<juce::uint8> (127)),
                       juce::jmax (0, blockSize - 1));
        eng.process (out, midi, ctx);
        for (int i = 0; i < out.getNumSamples(); ++i)
            REQUIRE (std::isfinite (out.getSample (0, i)));
    }
}
