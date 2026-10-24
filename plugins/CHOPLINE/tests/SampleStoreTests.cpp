// CHOPLINE tests — SampleStore: decode, resample, caps, caches, FLAC embed.

#include <catch2/catch_test_macros.hpp>

#include "TestHelpers.h"
#include "engine/SampleStore.h"

using namespace chopline;
using namespace chopline::test;

TEST_CASE ("Buffer load: resample to session rate, stereo duplication, caches",
           "[samplestore]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    engine::SampleStore store;
    const auto source = makeSine (440.0, 1.0, 44100.0);

    const auto error = store.loadBufferSync (source, 44100.0, 48000.0, "sine");
    REQUIRE (error.isEmpty());

    const auto data = store.current();
    REQUIRE (data != nullptr);

    // Resampled length within a sample of the exact ratio.
    const auto expected = static_cast<juce::int64> (std::llround (source.getNumSamples() * 48000.0 / 44100.0));
    REQUIRE (std::llabs (data->numFrames() - expected) <= 1);
    REQUIRE (juce::exactlyEqual (data->sessionRate, 48000.0));
    REQUIRE (juce::exactlyEqual (data->sourceRate, 44100.0));

    // Mono was duplicated to stereo.
    REQUIRE (data->audio.getNumChannels() == 2);
    REQUIRE (juce::exactlyEqual (data->audio.getSample (0, 1000), data->audio.getSample (1, 1000)));

    // Peak pyramid + onset envelope + FLAC blob exist.
    REQUIRE (data->peaksFine.bucketSize == 256);
    REQUIRE (data->peaksFine.mins.size() == static_cast<size_t> ((data->numFrames() + 255) / 256));
    REQUIRE (data->peaksCoarse.bucketSize == 4096);
    REQUIRE (! data->onsetEnvelope.empty());
    REQUIRE (data->flacData.getSize() > 0);

    // The waveform survived the trip (peak of a 0.8 sine ≈ 0.8).
    float mx = 0.0f;
    for (auto v : data->peaksFine.maxs)
        mx = juce::jmax (mx, v);
    REQUIRE (mx > 0.35f); // mono mix of a duplicated 0.8 sine stays ≈ 0.8; margin for resampling
}

TEST_CASE ("WAV file round trip", "[samplestore]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("chopline_test_sine.wav");
    temp.deleteFile();

    // Write a WAV.
    {
        const auto buf = makeSine (220.0, 0.5, 48000.0);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = temp.createOutputStream();
        REQUIRE (stream != nullptr);
        auto writer = wav.createWriterFor (stream,
                                           juce::AudioFormatWriterOptions{}
                                               .withSampleRate (48000.0)
                                               .withNumChannels (1)
                                               .withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        REQUIRE (writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples()));
    }

    engine::SampleStore store;
    const auto error = store.loadFileSync (temp, 48000.0);
    REQUIRE (error.isEmpty());

    const auto data = store.current();
    REQUIRE (data != nullptr);
    REQUIRE (data->numFrames() == static_cast<juce::int64> (0.5 * 48000.0));
    REQUIRE (data->fileHash.isNotEmpty());
    REQUIRE (data->filePath == temp.getFullPathName());

    temp.deleteFile();
}

TEST_CASE ("Samples longer than 5 minutes are rejected", "[samplestore]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    engine::SampleStore store;
    // 301 s at a low rate keeps the test fast.
    juce::AudioBuffer<float> longBuf (1, static_cast<int> (301.0 * 8000.0));
    longBuf.clear();

    const auto error = store.loadBufferSync (longBuf, 8000.0, 48000.0, "too-long");
    REQUIRE (error.isNotEmpty());
    REQUIRE (store.current() == nullptr);
}

TEST_CASE ("Missing file reports an error and keeps the previous sample",
           "[samplestore]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    engine::SampleStore store;
    REQUIRE (store.loadBufferSync (makeSine (440.0, 0.2, 48000.0), 48000.0, 48000.0).isEmpty());
    const auto before = store.current();
    REQUIRE (before != nullptr);

    const auto error = store.loadFileSync (juce::File ("/nonexistent/chopline-nope.wav"), 48000.0);
    REQUIRE (error.isNotEmpty());
    REQUIRE (store.current() == before); // old snapshot kept
}

TEST_CASE ("Embedded FLAC blob reloads identically (state embed path)", "[samplestore]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    engine::SampleStore store;
    REQUIRE (store.loadBufferSync (makeSine (330.0, 0.3, 48000.0), 48000.0, 48000.0).isEmpty());
    const auto first = store.current();
    REQUIRE (first != nullptr);

    engine::SampleStore second;
    REQUIRE (second.loadMemorySync (first->flacData, first->filePath, first->fileHash, 48000.0)
                 .isEmpty());
    const auto restored = second.current();
    REQUIRE (restored != nullptr);
    REQUIRE (restored->numFrames() == first->numFrames());

    // 24-bit FLAC round trip: sample-accurate to ~1e-4.
    for (int i = 0; i < restored->numFrames(); i += 997)
        REQUIRE (std::abs (restored->audio.getSample (0, i) - first->audio.getSample (0, i)) < 1.0e-4f);
}
