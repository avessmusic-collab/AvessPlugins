// CHOPLINE — SampleStore implementation. See header for the pipeline.

#include "SampleStore.h"

#include <juce_cryptography/juce_cryptography.h>

#include "TransientDetector.h"

#include <cmath>

namespace chopline::engine
{

namespace
{
    // Windowed-sinc resample of one channel (offline, background thread).
    void resampleChannel (const float* in, int numIn, float* out, int numOut, double speedRatio)
    {
        // The interpolator reads ceil(numOut * ratio) input samples plus its
        // internal history; give it a padded copy so it never reads past the end.
        const auto needed = static_cast<int> (std::ceil (static_cast<double> (numOut) * speedRatio)) + 64;
        std::vector<float> padded (static_cast<size_t> (juce::jmax (needed, numIn)), 0.0f);
        std::copy (in, in + numIn, padded.begin());

        juce::WindowedSincInterpolator interpolator;
        interpolator.reset();
        interpolator.process (speedRatio, padded.data(), out, numOut);
    }

    SampleData::PeakLevel buildPeakLevel (const juce::AudioBuffer<float>& audio, int bucketSize)
    {
        SampleData::PeakLevel level;
        level.bucketSize = bucketSize;

        const auto numSamples = audio.getNumSamples();
        const auto numBuckets = (numSamples + bucketSize - 1) / juce::jmax (1, bucketSize);
        level.mins.resize (static_cast<size_t> (numBuckets), 0.0f);
        level.maxs.resize (static_cast<size_t> (numBuckets), 0.0f);

        const auto* l = audio.getReadPointer (0);
        const auto* r = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : l;

        for (int b = 0; b < numBuckets; ++b)
        {
            const auto start = b * bucketSize;
            const auto end = juce::jmin (numSamples, start + bucketSize);
            float mn = 1.0e9f, mx = -1.0e9f;
            for (int i = start; i < end; ++i)
            {
                const auto v = 0.5f * (l[i] + r[i]);
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
            }
            level.mins[static_cast<size_t> (b)] = mn;
            level.maxs[static_cast<size_t> (b)] = mx;
        }
        return level;
    }

    juce::MemoryBlock encodeFlac (const juce::AudioBuffer<float>& audio, double sampleRate,
                                  juce::String& error)
    {
        juce::MemoryBlock block;
        juce::FlacAudioFormat flac;

        std::unique_ptr<juce::OutputStream> stream =
            std::make_unique<juce::MemoryOutputStream> (block, false);
        // 24-bit keeps the lossless copy audibly transparent.
        if (auto writer = flac.createWriterFor (stream,
                                                juce::AudioFormatWriterOptions{}
                                                    .withSampleRate (sampleRate)
                                                    .withNumChannels (audio.getNumChannels())
                                                    .withBitsPerSample (24)
                                                    .withQualityOptionIndex (5)))
        {
            if (! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
                error = "Could not encode the sample for state embedding";
            writer->flush();
        }
        else
        {
            error = "Could not create a FLAC encoder for state embedding";
        }
        return block;
    }
} // namespace

SampleStore::SampleStore()
{
    formats.registerBasicFormats();
}

SampleStore::~SampleStore()
{
    pool.removeAllJobs (true, 10000);
    masterReference.clear(); // any queued message-thread completion becomes a no-op
}

// ---- pipeline ---------------------------------------------------------------

std::unique_ptr<SampleData> SampleStore::buildFromReader (juce::AudioFormatReader& reader,
                                                          double sessionRate,
                                                          const juce::String& path,
                                                          const juce::String& hash,
                                                          juce::MemoryBlock existingFlac,
                                                          juce::String& error)
{
    if (reader.sampleRate <= 0.0 || reader.lengthInSamples <= 0)
    {
        error = "The file contains no audio";
        return nullptr;
    }

    const auto lengthSeconds = static_cast<double> (reader.lengthInSamples) / reader.sampleRate;
    if (lengthSeconds > maxLengthSeconds)
    {
        error = "Sample is longer than 5 minutes";
        return nullptr;
    }

    if (sessionRate <= 0.0)
        sessionRate = 48000.0;

    const auto numIn = static_cast<int> (reader.lengthInSamples);

    // Decode (first two channels; mono is duplicated to stereo).
    juce::AudioBuffer<float> source (2, numIn);
    if (! reader.read (&source, 0, numIn, 0, true, reader.numChannels > 1))
    {
        error = "Could not read the audio data";
        return nullptr;
    }
    if (reader.numChannels < 2)
        source.copyFrom (1, 0, source, 0, 0, numIn);

    auto data = std::make_unique<SampleData>();
    data->sourceRate = reader.sampleRate;
    data->sessionRate = sessionRate;
    data->filePath = path;
    data->fileName = path.isNotEmpty() ? juce::File (path).getFileName() : juce::String ("sample");
    data->fileHash = hash;

    // Lossless blob for state embedding and later re-resampling.
    if (existingFlac.getSize() > 0)
    {
        data->flacData = std::move (existingFlac);
    }
    else
    {
        juce::String flacError;
        data->flacData = encodeFlac (source, reader.sampleRate, flacError);
        // A failed embed is not fatal for playback; remember it, keep going.
        if (flacError.isNotEmpty())
            data->flacData.reset();
    }

    // Resample to the session rate.
    if (std::abs (data->sourceRate - sessionRate) < 0.01)
    {
        data->audio = std::move (source);
    }
    else
    {
        const auto ratio = data->sourceRate / sessionRate;
        const auto numOut = static_cast<int> (std::llround (static_cast<double> (numIn) / ratio));
        data->audio.setSize (2, juce::jmax (1, numOut));
        for (int ch = 0; ch < 2; ++ch)
            resampleChannel (source.getReadPointer (ch), numIn,
                             data->audio.getWritePointer (ch), data->audio.getNumSamples(), ratio);
    }

    // Display + analysis caches.
    data->peaksFine = buildPeakLevel (data->audio, 256);
    data->peaksCoarse = buildPeakLevel (data->audio, 4096);

    auto analysis = TransientDetector::analyze (data->audio, sessionRate);
    data->onsetEnvelope = std::move (analysis.envelope);
    data->onsetHopSeconds = analysis.hopSeconds;
    data->onsetFrameSeconds = analysis.frameSeconds;

    const TransientDetector::Analysis forBpm { data->onsetEnvelope,
                                               data->onsetHopSeconds,
                                               data->onsetFrameSeconds };
    data->detectedBpm = TransientDetector::estimateBpm (
        TransientDetector::pickOnsets (forBpm, 0.6f, 0.05), 120.0);

    return data;
}

std::unique_ptr<SampleData> SampleStore::buildFromFile (const juce::File& file,
                                                        double sessionRate,
                                                        juce::String& error)
{
    auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = file.existsAsFile() ? "Unsupported or unreadable audio file"
                                    : "File not found";
        return nullptr;
    }

    juce::String hash;
    if (auto stream = file.createInputStream())
        hash = juce::MD5 (*stream).toHexString();

    return buildFromReader (*reader, sessionRate, file.getFullPathName(), hash, {}, error);
}

std::unique_ptr<SampleData> SampleStore::buildFromMemory (juce::MemoryBlock flacBlob,
                                                          const juce::String& path,
                                                          const juce::String& hash,
                                                          double sessionRate,
                                                          juce::String& error)
{
    juce::FlacAudioFormat flac;
    auto reader = std::unique_ptr<juce::AudioFormatReader> (flac.createReaderFor (
        new juce::MemoryInputStream (flacBlob.getData(), flacBlob.getSize(), false), true));
    if (reader == nullptr)
    {
        error = "Embedded sample data is unreadable";
        return nullptr;
    }
    return buildFromReader (*reader, sessionRate, path, hash, std::move (flacBlob), error);
}

// ---- async plumbing ---------------------------------------------------------

void SampleStore::startJob (std::function<std::unique_ptr<SampleData> (juce::String&)> work)
{
    const auto jobId = ++jobCounter;
    loading.store (true);

    pool.addJob ([this, jobId, task = std::move (work)]
    {
        // ~SampleStore waits for the pool, so `this` is valid for the job body;
        // the message-thread hop below re-checks liveness via WeakReference.
        juce::String error;
        auto data = task (error);

        auto shared = std::shared_ptr<SampleData> (data.release());
        juce::WeakReference<SampleStore> weak (this);
        juce::MessageManager::callAsync ([weak, shared, error, jobId]() mutable
        {
            if (auto* self = weak.get())
                self->publishFinished (std::move (shared), error, jobId);
        });
    });
}

void SampleStore::loadFileAsync (const juce::File& file, double sessionRate)
{
    lastKnownSessionRate = sessionRate > 0.0 ? sessionRate : lastKnownSessionRate;
    const auto rate = lastKnownSessionRate;
    startJob ([this, file, rate] (juce::String& error) { return buildFromFile (file, rate, error); });
}

void SampleStore::loadMemoryAsync (juce::MemoryBlock flacBlob, juce::String originalPath,
                                   juce::String originalHash, double sessionRate)
{
    lastKnownSessionRate = sessionRate > 0.0 ? sessionRate : lastKnownSessionRate;
    const auto rate = lastKnownSessionRate;
    startJob ([this, blob = std::move (flacBlob), originalPath, originalHash, rate] (juce::String& error)
    {
        return buildFromMemory (blob, originalPath, originalHash, rate, error);
    });
}

void SampleStore::setSessionRate (double newRate)
{
    if (newRate <= 0.0)
        return;
    lastKnownSessionRate = newRate;

    auto cur = exchange.current();
    if (cur == nullptr || std::abs (cur->sessionRate - newRate) < 0.01)
        return;

    if (cur->flacData.getSize() > 0)
        loadMemoryAsync (cur->flacData, cur->filePath, cur->fileHash, newRate);
    else if (cur->filePath.isNotEmpty())
        loadFileAsync (juce::File (cur->filePath), newRate);
}

juce::String SampleStore::loadFileSync (const juce::File& file, double sessionRate)
{
    lastKnownSessionRate = sessionRate > 0.0 ? sessionRate : lastKnownSessionRate;
    juce::String error;
    auto data = buildFromFile (file, lastKnownSessionRate, error);
    publishFinished (std::shared_ptr<SampleData> (data.release()), error, ++jobCounter);
    return error;
}

juce::String SampleStore::loadBufferSync (const juce::AudioBuffer<float>& sourceIn,
                                          double sourceRate, double sessionRate,
                                          const juce::String& name)
{
    lastKnownSessionRate = sessionRate > 0.0 ? sessionRate : lastKnownSessionRate;

    // Round-trip through the FLAC encoder so the sync path exercises the same
    // pipeline as a real file load.
    juce::String error;
    juce::AudioBuffer<float> stereo (2, sourceIn.getNumSamples());
    stereo.copyFrom (0, 0, sourceIn, 0, 0, sourceIn.getNumSamples());
    stereo.copyFrom (1, 0, sourceIn, sourceIn.getNumChannels() > 1 ? 1 : 0, 0, sourceIn.getNumSamples());

    auto blob = encodeFlac (stereo, sourceRate, error);
    if (error.isNotEmpty())
        return error;

    auto data = buildFromMemory (std::move (blob), name, juce::MD5 (stereo.getReadPointer (0),
                                                                    static_cast<size_t> (stereo.getNumSamples()) * sizeof (float))
                                                             .toHexString(),
                                 lastKnownSessionRate, error);
    publishFinished (std::shared_ptr<SampleData> (data.release()), error, ++jobCounter);
    return error;
}

juce::String SampleStore::loadMemorySync (juce::MemoryBlock flacBlob, juce::String originalPath,
                                          juce::String originalHash, double sessionRate)
{
    lastKnownSessionRate = sessionRate > 0.0 ? sessionRate : lastKnownSessionRate;
    juce::String error;
    auto data = buildFromMemory (std::move (flacBlob), originalPath, originalHash,
                                 lastKnownSessionRate, error);
    publishFinished (std::shared_ptr<SampleData> (data.release()), error, ++jobCounter);
    return error;
}

void SampleStore::publishFinished (std::shared_ptr<SampleData> data, juce::String error, int jobId)
{
    // A newer request supersedes this result.
    if (jobId != jobCounter.load())
        return;

    loading.store (false);
    lastError = error;

    if (data != nullptr && error.isEmpty())
        exchange.publish (std::shared_ptr<const SampleData> (std::move (data)));

    if (onLoadFinished != nullptr)
        onLoadFinished (error);
}

void SampleStore::collectGarbage (const std::function<bool (const SampleData*)>& stillInUse)
{
    exchange.collectGarbage (stillInUse);
}

} // namespace chopline::engine
