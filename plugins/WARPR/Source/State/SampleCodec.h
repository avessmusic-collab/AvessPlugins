#pragma once
/*
  SampleCodec - the embedded-sample blob of the state container (architecture C17, "Container").

  The blob is everything AFTER the u32 sampleBlobByteCount of the container (see StateIO.h):

      u8   codec            0 = float32 planar raw (little-endian), 1 = FLAC 24-bit
      u8   numChannels      1 or 2
      u8   flags            bit0 = isDefaultWave (regenerated on load, no payload needed)
                            bit1 = periodic / single-cycle (circular guards and mips)
                            other bits are reserved and ignored on read
      f64  fileSampleRate   IEEE-754 binary64, little-endian
      u64  numFrames
      u16  nameByteCount    ; followed by that many bytes of UTF-8 sample name
      u32  payloadByteCount ; followed by the payload

  Codec choice (automatic): FLAC24 when EVERY sample of level 0 sits on the 24-bit grid k / 2^23 with
  k in [-2^23, 2^23 - 1] and none is a negative zero (that is exactly what 8 / 16 / 24-bit integer files
  decode to), otherwise float32.  A FLAC encode is verified by decoding it again; any mismatch falls back
  to float32, so the codec can never lose a bit.  The default wave is never embedded (encode() returns an
  empty block; the loader regenerates it).

  decode() is defensive: every length is validated against the bytes actually present BEFORE anything is
  allocated, so a corrupt or hostile blob can neither crash nor exhaust memory.

  BlobCache memoises the last encoding keyed on SampleData::serial, so repeated getStateInformation calls
  (hosts call it often) do not re-encode a multi-megabyte sample.
*/

#include "DSP/SampleData.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstddef>
#include <mutex>

namespace warpr::samplecodec
{
    enum class Codec : juce::uint8 { float32 = 0, flac24 = 1 };
    enum class Choice { automatic, forceFloat32 };
    enum class Status { ok, defaultWave, corrupt };

    inline constexpr juce::uint8 flagDefaultWave = 1;
    inline constexpr juce::uint8 flagPeriodic    = 2;
    inline constexpr std::size_t fixedHeaderBytes = 1 + 1 + 1 + 8 + 8 + 2;     // up to and including nameByteCount
    inline constexpr juce::uint64 maxTotalSamples = juce::uint64 (1) << 28;    // frames x channels (1 GiB of float)

    /** True when level 0 is exactly representable in 24-bit PCM (see the header comment). */
    bool isLossless24 (const SampleData& sample);

    /** Encodes `sample` into the blob.  Empty block for the default wave / an unusable sample. */
    juce::MemoryBlock encode (const SampleData& sample, Choice choice = Choice::automatic);

    /** Decodes a blob.  `status` = ok (sample returned), defaultWave (the default wave is returned) or corrupt (nullptr). */
    SampleData::Ptr decode (const void* data, std::size_t numBytes, Status& status);

    //==========================================================================
    class BlobCache
    {
    public:
        /** Thread-safe (host state thread / message thread).  Encodes at most once per SampleData. */
        juce::MemoryBlock blobFor (const SampleData::Ptr& sample);

        int getNumEncodes() const;

    private:
        mutable std::mutex mutex;
        juce::uint64 cachedSerial = 0;
        juce::MemoryBlock cached;
        int numEncodes = 0;
    };
}
