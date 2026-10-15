#pragma once

#include <array>
#include <chrono>
#include <cstdint>

#include "AnalysisSnapshot.h"

namespace prism
{
    // Compile-time capacity of the engine's per-source arrays. v1 activates only
    // kSourceCount (2): main input bus + sidechain bus.
    inline constexpr int kMaxSources = 16;

    // Monotonic milliseconds, shared by the processor (fetch heartbeat) and the worker.
    inline std::int64_t steadyNowMs() noexcept
    {
        using namespace std::chrono;
        return (std::int64_t) duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // Everything that forces the engine to be rebuilt when it changes.
    // busChannels[i] = channel count of input bus i as seen in the host buffer (0 = disabled/absent).
    struct EngineConfig
    {
        double sampleRate = 0.0;
        std::array<int, kMaxSources> busChannels {};

        bool operator==(const EngineConfig& o) const noexcept
        {
            return sampleRate == o.sampleRate && busChannels == o.busChannels;
        }
        bool operator!=(const EngineConfig& o) const noexcept { return ! (*this == o); }
    };

    struct SourceInfo
    {
        int  busIndex      = -1;
        int  channelOffset = 0;   // first channel of this bus inside the host's shared buffer
        int  numChannels   = 0;   // 1 or 2 when active
        bool active        = false;
    };

    // Maps host input buses to analysis sources (architecture C1).
    // v1: source i == input bus i, i < kSourceCount; only mono/stereo buses are analysed.
    class SourceRegistry
    {
    public:
        void build(const EngineConfig& cfg) noexcept
        {
            activeCount = 0;
            int offset = 0;
            for (int i = 0; i < kMaxSources; ++i)
            {
                SourceInfo& s = sources[(size_t) i];
                s = {};
                s.busIndex = i;
                s.channelOffset = offset;
                const int ch = cfg.busChannels[(size_t) i];
                offset += ch;
                if (i < kSourceCount && (ch == 1 || ch == 2))
                {
                    s.numChannels = ch;
                    s.active = true;
                    activeIndices[(size_t) activeCount++] = i;
                }
            }
        }

        const SourceInfo& operator[](int i) const noexcept { return sources[(size_t) i]; }
        int getActiveCount() const noexcept { return activeCount; }
        int getActiveIndex(int n) const noexcept { return activeIndices[(size_t) n]; }

    private:
        std::array<SourceInfo, kMaxSources> sources {};
        std::array<int, kMaxSources> activeIndices {};
        int activeCount = 0;
    };
} // namespace prism
