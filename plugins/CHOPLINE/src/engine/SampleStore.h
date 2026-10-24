// CHOPLINE — SampleStore (spec §3, §4)
// Decoded sample audio: loaded and resampled on a background thread,
// published to the audio thread as immutable, ref-counted snapshots via
// SnapshotExchange (atomic pointer publication, deferred reclamation on the
// message thread).
//
// Pipeline per load (background thread):
//   decode (WAV/AIFF/FLAC/MP3/OGG via AudioFormatManager, ≤ 5 min, ≤ 2 ch)
//   → lossless FLAC blob (state embedding + later re-resampling)
//   → windowed-sinc resample to the session rate
//   → waveform peak pyramid (mono min/max, buckets 256 and 4096)
//   → spectral-flux onset envelope + rough BPM estimate
//   → publish on the message thread.
//
// A session-rate change re-runs the pipeline from the cached FLAC blob, so
// the original file is only read once (and the sample survives the file
// disappearing).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "SnapshotExchange.h"

namespace chopline::engine
{

struct SampleData
{
    juce::AudioBuffer<float> audio; // session rate, always 2 channels
    double sessionRate = 0.0;
    double sourceRate = 0.0;
    juce::String filePath, fileName;
    juce::String fileHash; // MD5 of the source file bytes (relink identity)
    double detectedBpm = 120.0;
    juce::MemoryBlock flacData; // lossless 24-bit source-rate copy

    struct PeakLevel
    {
        int bucketSize = 0;
        std::vector<float> mins, maxs; // mono mix min/max per bucket
    };
    PeakLevel peaksFine;   // bucket 256
    PeakLevel peaksCoarse; // bucket 4096

    std::vector<float> onsetEnvelope; // normalised, one value per analysis hop
    double onsetHopSeconds = 0.0;
    double onsetFrameSeconds = 0.0;

    juce::int64 numFrames() const noexcept { return audio.getNumSamples(); }
    double lengthSeconds() const noexcept
    {
        return sessionRate > 0.0 ? static_cast<double> (numFrames()) / sessionRate : 0.0;
    }
};

class SampleStore final
{
public:
    static constexpr double maxLengthSeconds = 300.0; // 5 minutes (spec §4)

    SampleStore();
    ~SampleStore();

    /// Message thread. Called with an empty string on success.
    std::function<void (juce::String error)> onLoadFinished;

    // ---- message thread ----------------------------------------------------
    void loadFileAsync (const juce::File& file, double sessionRate);
    void loadMemoryAsync (juce::MemoryBlock flacBlob,
                          juce::String originalPath,
                          juce::String originalHash,
                          double sessionRate);
    /// Re-resamples the current sample from its FLAC blob when the rate changed.
    void setSessionRate (double newRate);

    bool isLoading() const noexcept { return loading.load(); }
    juce::String getLastError() const { return lastError; }

    std::shared_ptr<const SampleData> current() const { return exchange.current(); }
    void collectGarbage (const std::function<bool (const SampleData*)>& stillInUse);

    /// Synchronous variants (tests / callers already on the message thread).
    juce::String loadFileSync (const juce::File& file, double sessionRate);
    juce::String loadBufferSync (const juce::AudioBuffer<float>& source,
                                 double sourceRate,
                                 double sessionRate,
                                 const juce::String& name = "buffer");
    juce::String loadMemorySync (juce::MemoryBlock flacBlob,
                                 juce::String originalPath,
                                 juce::String originalHash,
                                 double sessionRate);

    // ---- audio thread -------------------------------------------------------
    const SnapshotExchange<SampleData>::Node* acquire() const noexcept { return exchange.acquire(); }
    void ack (const SnapshotExchange<SampleData>::Node* node) noexcept { exchange.ack (node); }

private:
    std::unique_ptr<SampleData> buildFromReader (juce::AudioFormatReader& reader,
                                                 double sessionRate,
                                                 const juce::String& path,
                                                 const juce::String& hash,
                                                 juce::MemoryBlock existingFlac,
                                                 juce::String& error);
    std::unique_ptr<SampleData> buildFromFile (const juce::File&, double sessionRate,
                                               juce::String& error);
    std::unique_ptr<SampleData> buildFromMemory (juce::MemoryBlock flacBlob,
                                                 const juce::String& path,
                                                 const juce::String& hash,
                                                 double sessionRate,
                                                 juce::String& error);

    void startJob (std::function<std::unique_ptr<SampleData> (juce::String& error)> work);
    void publishFinished (std::shared_ptr<SampleData> data, juce::String error, int jobId);

    juce::AudioFormatManager formats;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (1)
                                                     .withThreadName ("CHOPLINE Sample Load") };
    SnapshotExchange<SampleData> exchange;
    std::atomic<bool> loading { false };
    std::atomic<int> jobCounter { 0 };
    juce::String lastError; // message thread
    double lastKnownSessionRate = 48000.0;

    JUCE_DECLARE_WEAK_REFERENCEABLE (SampleStore)
    JUCE_DECLARE_NON_COPYABLE (SampleStore)
};

} // namespace chopline::engine
