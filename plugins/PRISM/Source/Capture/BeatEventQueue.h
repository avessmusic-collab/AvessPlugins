#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "../Analysis/AnalysisSnapshot.h"   // kSourceCount

namespace prism
{
    struct BeatEvent
    {
        // Sentinel for sourceFrame[]: "no per-source mapping was recorded, use frameIndex" (events pushed by hand in tests).
        static constexpr std::int64_t kUnmapped = INT64_MIN;

        std::uint64_t frameIndex = 0;   // absolute index in the SOURCE-0 ring stream (== source-0 worker framesSeen domain);
                                        // this is what barAnchorStreamFrame (the page's grid anchor, anchored to source 0) uses
        bool          isBar      = false;

        // The same instant expressed in each source's OWN stream (frames written to that source's ring so far + the
        // offset inside the block). Streams diverge when a source's ring drops a block (overrun) or a source is skipped
        // (undersized host buffer): the frames of that block simply do not exist in that source's stream, so its column
        // clock falls behind source 0's for the rest of the session. -1 = this block is not part of that source's stream
        // (dropped / skipped / source inactive): the event has no position there and is not applied to it.
        std::array<std::int64_t, kSourceCount> sourceFrame { kUnmapped, kUnmapped };
    };

    // Tiny wait-free SPSC queue (audio thread pushes, worker pops). Capacity 256, drop on full.
    class BeatEventQueue
    {
    public:
        static constexpr int kCapacity = 256;   // power of two

        bool push(const BeatEvent& e) noexcept
        {
            const auto w = writeIdx.load(std::memory_order_relaxed);
            const auto r = readIdx.load(std::memory_order_acquire);
            if (w - r >= (std::uint32_t) kCapacity)
            {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            slots[w & (kCapacity - 1)] = e;
            writeIdx.store(w + 1, std::memory_order_release);
            return true;
        }

        bool pop(BeatEvent& out) noexcept
        {
            const auto r = readIdx.load(std::memory_order_relaxed);
            const auto w = writeIdx.load(std::memory_order_acquire);
            if (r == w)
                return false;
            out = slots[r & (kCapacity - 1)];
            readIdx.store(r + 1, std::memory_order_release);
            return true;
        }

        void reset() noexcept
        {
            writeIdx.store(0, std::memory_order_relaxed);
            readIdx.store(0, std::memory_order_relaxed);
            dropped.store(0, std::memory_order_relaxed);
        }

        std::uint32_t getDroppedCount() const noexcept { return dropped.load(std::memory_order_relaxed); }

    private:
        std::array<BeatEvent, kCapacity> slots {};
        alignas(64) std::atomic<std::uint32_t> writeIdx { 0 };
        alignas(64) std::atomic<std::uint32_t> readIdx { 0 };
        std::atomic<std::uint32_t> dropped { 0 };
    };
} // namespace prism
