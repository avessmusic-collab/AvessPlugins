#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace prism
{
    // Worker-private random-access history of the last 65536 frames of one source
    // (architecture C4). No atomics. Always stores 2 channels; a mono source is
    // stored with R == L (consumers use numChannels() to know it is mono).
    class StreamHistory
    {
    public:
        static constexpr int kCapacity = 65536;   // power of two

        struct Segments
        {
            const float* a = nullptr; int na = 0;   // older part
            const float* b = nullptr; int nb = 0;   // newer part (nb may be 0)
            int total() const noexcept { return na + nb; }
        };

        void prepare(int sourceChannels)
        {
            srcChannels = sourceChannels;
            data[0].assign(kCapacity, 0.0f);
            data[1].assign(kCapacity, 0.0f);
            reset();
        }

        void release()
        {
            std::vector<float>().swap(data[0]);
            std::vector<float>().swap(data[1]);
            srcChannels = 0;
            reset();
        }

        void reset() noexcept { seen = 0; nonFinite = 0; }

        int numChannels() const noexcept { return srcChannels; }
        std::uint64_t framesSeen() const noexcept { return seen; }
        std::uint64_t nonFiniteSanitized() const noexcept { return nonFinite; }

        // src[0] (and src[1] if stereo). Non-finite values are replaced by 0 and counted.
        void push(const float* const* src, int n) noexcept
        {
            if (srcChannels <= 0 || n <= 0)
                return;
            for (int i = 0; i < n; ++i)
            {
                const std::size_t pos = (std::size_t) ((seen + (std::uint64_t) i) & (kCapacity - 1));
                const float l = sanitize(src[0][i]);
                data[0][pos] = l;
                data[1][pos] = srcChannels > 1 ? sanitize(src[1][i]) : l;
            }
            seen += (std::uint64_t) n;
        }

        // Most recent n frames of channel c (0/1) as two contiguous segments (oldest first).
        // n is clamped to min(framesSeen, capacity).
        Segments latest(int channel, int n) const noexcept
        {
            Segments s;
            if (srcChannels <= 0)
                return s;
            const std::uint64_t avail = std::min<std::uint64_t>(seen, kCapacity);
            const int count = (int) std::min<std::uint64_t>((std::uint64_t) std::max(0, n), avail);
            if (count == 0)
                return s;

            const std::size_t startAbs = (std::size_t) ((seen - (std::uint64_t) count) & (kCapacity - 1));
            const float* base = data[channel & 1].data();
            const int firstLen = (int) std::min<std::size_t>((std::size_t) count, kCapacity - startAbs);
            s.a = base + startAbs;  s.na = firstLen;
            s.b = base;             s.nb = count - firstLen;
            return s;
        }

        // Copies frames [startAbs, startAbs + n) of a channel (absolute stream indices) into dst.
        // Returns false if the range is not (or no longer) available.
        bool copyAbsolute(int channel, std::uint64_t startAbs, int n, float* dst) const noexcept
        {
            if (srcChannels <= 0 || n <= 0 || startAbs + (std::uint64_t) n > seen || seen - startAbs > (std::uint64_t) kCapacity)
                return false;
            const float* base = data[channel & 1].data();
            const std::size_t s = (std::size_t) (startAbs & (kCapacity - 1));
            const std::size_t first = std::min<std::size_t>((std::size_t) n, kCapacity - s);
            std::memcpy(dst, base + s, first * sizeof(float));
            if (first < (std::size_t) n)
                std::memcpy(dst + first, base, ((std::size_t) n - first) * sizeof(float));
            return true;
        }

    private:
        float sanitize(float x) noexcept
        {
            std::uint32_t bits;
            std::memcpy(&bits, &x, sizeof(bits));
            if ((bits & 0x7f800000u) == 0x7f800000u)
            {
                ++nonFinite;
                return 0.0f;
            }
            return x;
        }

        std::vector<float> data[2];
        int srcChannels = 0;
        std::uint64_t seen = 0;
        std::uint64_t nonFinite = 0;
    };
} // namespace prism
