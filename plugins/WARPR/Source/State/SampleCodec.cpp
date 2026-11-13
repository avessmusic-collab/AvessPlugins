#include "State/SampleCodec.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace warpr::samplecodec
{
    namespace
    {
        constexpr double kGrid24 = 8388608.0;                 // 2^23
        constexpr int kFlacChunk = 1 << 16;                    // frames per writer call (bounds the writer's temporary int arrays)

        std::size_t frameCount (const SampleData& s) noexcept   { return static_cast<std::size_t> (s.numFrames); }
        std::size_t chanCount (const SampleData& s) noexcept    { return static_cast<std::size_t> (s.numChannels); }

        void encodeFloat32 (const SampleData& s, juce::MemoryBlock& payload)
        {
            const auto frames = frameCount (s);
            const auto chans  = chanCount (s);
            payload.setSize (frames * chans * sizeof (float));
            auto* dst = static_cast<juce::uint8*> (payload.getData());

            for (std::size_t c = 0; c < chans; ++c)
            {
                const float* src = s.level (0).channel (static_cast<int> (c));

                for (std::size_t i = 0; i < frames; ++i)
                {
                    juce::uint32 bits = 0;
                    std::memcpy (&bits, &src[i], sizeof (bits));
                    bits = juce::ByteOrder::swapIfBigEndian (bits);            // wire format is little-endian
                    std::memcpy (dst + (c * frames + i) * sizeof (bits), &bits, sizeof (bits));
                }
            }
        }

        bool decodeFloat32 (const juce::uint8* src, std::size_t numBytes, int chans, int frames, juce::AudioBuffer<float>& out)
        {
            const auto f = static_cast<std::size_t> (frames);
            const auto c = static_cast<std::size_t> (chans);

            if (numBytes != f * c * sizeof (float))
                return false;

            out.setSize (chans, frames);

            for (std::size_t ch = 0; ch < c; ++ch)
            {
                float* dst = out.getWritePointer (static_cast<int> (ch));

                for (std::size_t i = 0; i < f; ++i)
                {
                    juce::uint32 bits = 0;
                    std::memcpy (&bits, src + (ch * f + i) * sizeof (bits), sizeof (bits));
                    bits = juce::ByteOrder::swapIfBigEndian (bits);
                    std::memcpy (&dst[i], &bits, sizeof (bits));
                }
            }

            return true;
        }

        bool decodeFlac (const void* data, std::size_t numBytes, int chans, int frames, juce::AudioBuffer<float>& out)
        {
            juce::FlacAudioFormat flac;
            std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (new juce::MemoryInputStream (data, numBytes, false), true));

            if (reader == nullptr || reader->lengthInSamples != static_cast<juce::int64> (frames)
                || static_cast<int> (reader->numChannels) != chans)
                return false;

            out.setSize (chans, frames);
            return reader->read (&out, 0, frames, 0, true, chans == 2);
        }

        bool encodeFlac24 (const SampleData& s, juce::MemoryBlock& payload)
        {
            const int frames = s.numFrames;
            const int chans = s.numChannels;

            juce::AudioBuffer<float> source (chans, frames);
            for (int c = 0; c < chans; ++c)
                source.copyFrom (c, 0, s.level (0).channel (c), frames);

            const double rate = juce::jlimit (1.0, 655350.0, std::round (s.fileSampleRate));
            bool wrote = false;

            {
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (payload, false);
                juce::FlacAudioFormat flac;
                auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                .withSampleRate (rate)
                                                                .withNumChannels (chans)
                                                                .withBitsPerSample (24));
                wrote = writer != nullptr;

                // NOT writeFromAudioSampleBuffer: it converts float -> int32 with round(INT_MAX * x), which is one
                // LSB (of 32 bit) short for |x| > 0.5 and the FLAC writer then shifts that down to k - 1.  The samples
                // are on the 24-bit grid, so hand the writer the exact left-justified integers k * 256 instead.
                std::vector<int> ints (static_cast<std::size_t> (chans) * static_cast<std::size_t> (kFlacChunk));

                for (int start = 0; wrote && start < frames; start += kFlacChunk)
                {
                    const int n = juce::jmin (kFlacChunk, frames - start);
                    const int* channelPointers[SampleData::maxChannels] = {};

                    for (int c = 0; c < chans; ++c)
                    {
                        int* dst = ints.data() + static_cast<std::size_t> (c) * static_cast<std::size_t> (kFlacChunk);
                        const float* src = source.getReadPointer (c) + start;

                        for (int i = 0; i < n; ++i)
                            dst[i] = static_cast<int> (src[i] * 8388608.0f) * 256;     // exact: |k| <= 2^23, power-of-two scale

                        channelPointers[c] = dst;
                    }

                    wrote = writer->write (channelPointers, n);
                }
            }                                                                   // writer destroyed -> stream finalised

            if (! wrote || payload.getSize() == 0)
                return false;

            // Verify: the codec must never lose a bit.  Anything else falls back to float32.
            juce::AudioBuffer<float> check;

            if (! decodeFlac (payload.getData(), payload.getSize(), chans, frames, check))
                return false;

            for (int c = 0; c < chans; ++c)
                if (std::memcmp (check.getReadPointer (c), source.getReadPointer (c), static_cast<std::size_t> (frames) * sizeof (float)) != 0)
                    return false;

            return true;
        }

        bool plausibleSampleRate (double sr) noexcept
        {
            return std::isfinite (sr) && sr > 0.0 && sr <= 1.0e9;
        }
    }

    //==========================================================================
    bool isLossless24 (const SampleData& s)
    {
        for (int c = 0; c < s.numChannels; ++c)
        {
            const float* d = s.level (0).channel (c);

            for (int i = 0; i < s.numFrames; ++i)
            {
                const float v = d[i];

                if (std::signbit (v) && juce::exactlyEqual (v, 0.0f))
                    return false;                                   // -0.0 would come back as +0.0

                const double scaled = static_cast<double> (v) * kGrid24;

                if (! (scaled >= -kGrid24 && scaled <= kGrid24 - 1.0) || ! juce::exactlyEqual (std::floor (scaled), scaled))
                    return false;                                   // off the grid, +1.0, NaN or out of range
            }
        }

        return true;
    }

    juce::MemoryBlock encode (const SampleData& s, Choice choice)
    {
        if (s.isDefaultWave || s.numChannels < 1 || s.numChannels > SampleData::maxChannels || s.numFrames <= 0
            || static_cast<juce::uint64> (s.numFrames) * static_cast<juce::uint64> (s.numChannels) > maxTotalSamples)
            return {};

        juce::MemoryBlock payload;
        Codec codec = Codec::float32;

        if (choice == Choice::automatic && isLossless24 (s))
        {
            if (encodeFlac24 (s, payload))
                codec = Codec::flac24;
            else
                payload.reset();
        }

        if (codec == Codec::float32)
            encodeFloat32 (s, payload);

        // UTF-8 name, truncated at a character boundary to the u16 field
        juce::String name = s.name;
        while (name.getNumBytesAsUTF8() > 65535)
            name = name.dropLastCharacters (juce::jmax (1, static_cast<int> ((name.getNumBytesAsUTF8() - 65535) / 4)));

        const auto nameBytes = name.getNumBytesAsUTF8();

        juce::MemoryOutputStream out;
        out.writeByte (static_cast<char> (codec));
        out.writeByte (static_cast<char> (s.numChannels));
        out.writeByte (static_cast<char> (s.singleCycle ? flagPeriodic : juce::uint8 (0)));
        out.writeDouble (s.fileSampleRate);
        out.writeInt64 (static_cast<juce::int64> (s.numFrames));
        out.writeShort (static_cast<short> (static_cast<juce::uint16> (nameBytes)));
        out.write (name.toRawUTF8(), nameBytes);
        out.writeInt (static_cast<int> (static_cast<juce::uint32> (payload.getSize())));
        out.write (payload.getData(), payload.getSize());
        return out.getMemoryBlock();
    }

    SampleData::Ptr decode (const void* data, std::size_t numBytes, Status& status)
    {
        status = Status::corrupt;

        if (data == nullptr || numBytes < fixedHeaderBytes)
            return nullptr;

        const auto* bytes = static_cast<const juce::uint8*> (data);
        juce::MemoryInputStream in (data, numBytes, false);

        const auto codec = static_cast<juce::uint8> (in.readByte());
        const int chans  = static_cast<juce::uint8> (in.readByte());
        const auto flags = static_cast<juce::uint8> (in.readByte());
        const double sampleRate = in.readDouble();
        const juce::int64 frames64 = in.readInt64();
        const auto nameBytes = static_cast<std::size_t> (static_cast<juce::uint16> (in.readShort()));

        if ((flags & flagDefaultWave) != 0)
        {
            status = Status::defaultWave;                           // never embedded; regenerated deterministically
            return SampleData::createDefaultWave();
        }

        if (chans < 1 || chans > SampleData::maxChannels || ! plausibleSampleRate (sampleRate)
            || frames64 <= 0 || static_cast<juce::uint64> (frames64) * static_cast<juce::uint64> (chans) > maxTotalSamples)
            return nullptr;

        std::size_t pos = fixedHeaderBytes;

        if (nameBytes + sizeof (juce::uint32) > numBytes - pos)
            return nullptr;

        const auto name = juce::String::fromUTF8 (reinterpret_cast<const char*> (bytes + pos), static_cast<int> (nameBytes));
        pos += nameBytes;

        const auto payloadBytes = static_cast<std::size_t> (juce::ByteOrder::littleEndianInt (bytes + pos));
        pos += sizeof (juce::uint32);

        if (payloadBytes > numBytes - pos)
            return nullptr;

        const int frames = static_cast<int> (frames64);
        juce::AudioBuffer<float> audio;
        bool decoded = false;

        if (codec == static_cast<juce::uint8> (Codec::float32))
            decoded = decodeFloat32 (bytes + pos, payloadBytes, chans, frames, audio);
        else if (codec == static_cast<juce::uint8> (Codec::flac24))
            decoded = decodeFlac (bytes + pos, payloadBytes, chans, frames, audio);

        if (! decoded)
            return nullptr;

        auto sample = SampleData::create (audio, sampleRate, name, (flags & flagPeriodic) != 0, false);

        if (sample == nullptr)
            return nullptr;

        status = Status::ok;
        return sample;
    }

    //==========================================================================
    juce::MemoryBlock BlobCache::blobFor (const SampleData::Ptr& sample)
    {
        if (sample == nullptr || sample->isDefaultWave)
            return {};

        const std::scoped_lock lock (mutex);

        if (sample->serial == 0 || sample->serial != cachedSerial)
        {
            cached = encode (*sample);
            cachedSerial = sample->serial;
            ++numEncodes;
        }

        return cached;
    }

    int BlobCache::getNumEncodes() const
    {
        const std::scoped_lock lock (mutex);
        return numEncodes;
    }
}
