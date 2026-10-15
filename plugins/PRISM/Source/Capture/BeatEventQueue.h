#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace prism
{
    struct BeatEvent
    {
        std::uint64_t frameIndex = 0;   // absolute index in the source-0 ring stream (== worker framesSeen domain)
        bool          isBar      = false;
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
