// CHOPLINE — ChopVoice (spec §3, §4)
// One playback voice. Phase 1: plain (unwarped) stereo playback of a chop's
// sample range with non-destructive equal-power declick fades at the chop
// start and end, velocity gain, and a 2 ms fast-fade used for voice stealing.
//
// Retriggering an active voice never clicks: the running stream is moved into
// an internal TAIL stream that fast-fades out (2 ms) while the new note
// starts immediately on the main stream — this IS the "steal oldest with a
// 2 ms fade" behaviour (spec §3).
//
// Snapshot safety: each stream publishes the SampleData* it reads through an
// atomic, so the message-thread garbage collector never frees a sample
// snapshot a voice is still playing (see SnapshotExchange / Engine).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "SampleStore.h"
#include "SliceMap.h"

#include <atomic>

namespace chopline::engine
{

class ChopVoice final
{
public:
    static constexpr double fastFadeSeconds = 0.002; // 2 ms steal fade

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;

    // ---- audio thread -------------------------------------------------------
    void trigger (const SampleData* sample, const SliceMapSnapshot::Chop& chop,
                  int chopIndex, float gain, std::uint64_t newStamp) noexcept;
    void fastRelease() noexcept; // steal / all-notes-off: 2 ms fade
    void renderAdd (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept;

    bool isActive() const noexcept { return main.active || tail.active; }
    bool isHolding() const noexcept { return main.active && ! main.fastFading; }
    std::uint64_t getStamp() const noexcept { return stamp; }

    // ---- message thread (UI feedback / snapshot GC) --------------------------
    int getUiChopIndex() const noexcept { return uiChop.load (std::memory_order_relaxed); }
    float getUiNormalisedPos() const noexcept { return uiPos.load (std::memory_order_relaxed); }
    bool usesSample (const SampleData* s) const noexcept
    {
        return main.sample.load (std::memory_order_relaxed) == s
            || tail.sample.load (std::memory_order_relaxed) == s;
    }

private:
    struct Stream
    {
        std::atomic<const SampleData*> sample { nullptr };
        juce::int64 pos = 0, start = 0, end = 0;
        int fadeIn = 0, fadeOut = 0;
        float gain = 0.0f;
        float fastGain = 1.0f;
        bool fastFading = false;
        bool active = false;

        void stop() noexcept
        {
            active = false;
            fastFading = false;
            sample.store (nullptr, std::memory_order_relaxed);
        }
        void renderAdd (juce::AudioBuffer<float>& out, int startSample, int numSamples,
                        float fadeStep) noexcept;
    };

    static void copyStream (Stream& dst, const Stream& src) noexcept;

    Stream main, tail;
    float fastFadeStep = 1.0f / 96.0f;
    std::uint64_t stamp = 0;
    std::atomic<int> uiChop { -1 };
    std::atomic<float> uiPos { 0.0f };
};

} // namespace chopline::engine
