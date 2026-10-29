// CHOPLINE — DetHash (Phase 5, spec §5/§8)
// Deterministic position-keyed hashing for everything "random" that must
// reproduce in an offline bounce: per-step probability rolls, Beat Repeat
// AUTO chance, and the granular/freeze grain jitter streams.
//
// A bounce and a live pass over the same timeline hash the same keys, so the
// result is identical — unlike a running RNG, whose state depends on how many
// rolls happened before. SplitMix64 finaliser: stable across platforms,
// no allocation, audio-thread safe.

#pragma once

#include <cstdint>

namespace chopline::engine
{

/// SplitMix64 finaliser — a high-quality 64-bit mixer.
inline constexpr std::uint64_t detMix (std::uint64_t x) noexcept
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/// Order-dependent combination of up to four keys.
inline constexpr std::uint64_t detHash (std::uint64_t a, std::uint64_t b,
                                        std::uint64_t c = 0, std::uint64_t d = 0) noexcept
{
    auto h = detMix (a);
    h = detMix (h ^ b);
    h = detMix (h ^ c);
    return detMix (h ^ d);
}

/// Uniform float in [0, 1) from a hash.
inline constexpr float det01 (std::uint64_t h) noexcept
{
    return static_cast<float> (h >> 40) * (1.0f / 16777216.0f);
}

/// Tiny deterministic RNG stream (SplitMix64) for grain jitter. Seeded per
/// trigger from deterministic keys, so two identical renders jitter alike.
struct DetRng
{
    std::uint64_t state = 0;

    void seed (std::uint64_t s) noexcept { state = detMix (s); }

    std::uint64_t nextU64() noexcept
    {
        state += 0x9e3779b97f4a7c15ull;
        return detMix (state);
    }

    /// [0, 1)
    float next01() noexcept { return det01 (nextU64()); }

    /// [-1, 1)
    float nextBipolar() noexcept { return next01() * 2.0f - 1.0f; }
};

} // namespace chopline::engine
