#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace prism
{
    // Wait-free single-producer / single-consumer frame ring (architecture C2).
    //
    //  - Planar storage in ONE allocation: float[channels][capacity], capacity = 2^log2 frames.
    //  - Producer (audio thread) owns writeIdx (release-store AFTER the copy);
    //    consumer (worker) owns readIdx (release-store after it finished reading).
    //  - Overrun policy: if the whole block does not fit, the WHOLE block is dropped,
    //    overrunCount++ (relaxed) and droppedFrames += n. Never blocks, never overwrites
    //    unread data, never writes a partial block (so frames are never torn).
    //
    // prepare()/reset() allocate / rewind and may only be called while neither side runs.
    class SpscFrameRing
    {
    public:
        static constexpr int kDefaultLog2Capacity = 18;   // 2^18 = 262144 frames

        void prepare(int numChannelsIn, int log2Capacity = kDefaultLog2Capacity)
        {
            channels = numChannelsIn;
            capacity = (std::uint64_t) 1 << log2Capacity;
            mask     = capacity - 1;
            storage.assign((size_t) channels * (size_t) capacity, 0.0f);
            reset();
        }

        void release()
        {
            std::vector<float>().swap(storage);
            channels = 0;
            capacity = mask = 0;
            reset();
        }

        void reset() noexcept
        {
            writeIdx.store(0, std::memory_order_relaxed);
            readIdx.store(0, std::memory_order_relaxed);
            overruns.store(0, std::memory_order_relaxed);
            dropped.store(0, std::memory_order_relaxed);
            lastDropPos.store(0, std::memory_order_relaxed);
        }

        int  getNumChannels() const noexcept { return channels; }
        std::uint64_t getCapacity() const noexcept { return capacity; }
        bool isPrepared() const noexcept { return channels > 0; }

        //======================================================================
        // PRODUCER (audio thread). src must hold >= getNumChannels() pointers.
        // Returns false if the block was dropped.
        bool write(const float* const* src, int numFrames) noexcept
        {
            if (numFrames <= 0)
                return true;
            if (channels <= 0)
                return false;

            const std::uint64_t w = writeIdx.load(std::memory_order_relaxed);
            const std::uint64_t r = readIdx.load(std::memory_order_acquire);
            const std::uint64_t used = w - r;
            const std::uint64_t n = (std::uint64_t) numFrames;

            if (n > capacity || capacity - used < n)
            {
                dropped.fetch_add(n, std::memory_order_relaxed);
                lastDropPos.store(w, std::memory_order_relaxed);        // stream position of the discontinuity
                overruns.fetch_add(1, std::memory_order_release);       // publishes lastDropPos to the reader
                return false;
            }

            const std::uint64_t start = w & mask;
            const std::uint64_t first = std::min(n, capacity - start);
            for (int c = 0; c < channels; ++c)
            {
                float* dst = storage.data() + (size_t) c * (size_t) capacity;
                std::memcpy(dst + start, src[c], (size_t) first * sizeof(float));
                if (first < n)
                    std::memcpy(dst, src[c] + first, (size_t) (n - first) * sizeof(float));
            }

            writeIdx.store(w + n, std::memory_order_release);
            return true;
        }

        // Absolute index of the next frame the producer will write (producer-side read).
        std::uint64_t producerPosition() const noexcept { return writeIdx.load(std::memory_order_relaxed); }

        //======================================================================
        // CONSUMER (worker).
        int readAvailable() const noexcept
        {
            return (int) (writeIdx.load(std::memory_order_acquire) - readIdx.load(std::memory_order_relaxed));
        }

        // Copies up to maxFrames into dst[c] (>= getNumChannels() pointers). Returns frames read.
        int read(float* const* dst, int maxFrames) noexcept
        {
            const std::uint64_t r = readIdx.load(std::memory_order_relaxed);
            const std::uint64_t w = writeIdx.load(std::memory_order_acquire);
            const std::uint64_t n = std::min<std::uint64_t>(w - r, (std::uint64_t) std::max(0, maxFrames));
            if (n == 0)
                return 0;

            const std::uint64_t start = r & mask;
            const std::uint64_t first = std::min(n, capacity - start);
            for (int c = 0; c < channels; ++c)
            {
                const float* srcCh = storage.data() + (size_t) c * (size_t) capacity;
                std::memcpy(dst[c], srcCh + start, (size_t) first * sizeof(float));
                if (first < n)
                    std::memcpy(dst[c] + first, srcCh, (size_t) (n - first) * sizeof(float));
            }

            readIdx.store(r + n, std::memory_order_release);
            return (int) n;
        }

        // Acquire-load of the counter pairs with the release in write(): getLastDropPos() is then current.
        std::uint64_t getOverrunCount() const noexcept  { return overruns.load(std::memory_order_acquire); }
        // Stream position (frames written so far) at the most recent drop.
        std::uint64_t getLastDropPos() const noexcept   { return lastDropPos.load(std::memory_order_relaxed); }
        std::uint64_t getDroppedFrames() const noexcept { return dropped.load(std::memory_order_relaxed); }
        std::uint64_t getTotalWritten() const noexcept  { return writeIdx.load(std::memory_order_acquire); }
        std::uint64_t getTotalRead() const noexcept     { return readIdx.load(std::memory_order_acquire); }

    private:
        std::vector<float> storage;
        int channels = 0;
        std::uint64_t capacity = 0, mask = 0;

        alignas(64) std::atomic<std::uint64_t> writeIdx { 0 };
        alignas(64) std::atomic<std::uint64_t> readIdx { 0 };
        alignas(64) std::atomic<std::uint64_t> overruns { 0 };
        std::atomic<std::uint64_t> dropped { 0 };
        std::atomic<std::uint64_t> lastDropPos { 0 };
    };
} // namespace prism
