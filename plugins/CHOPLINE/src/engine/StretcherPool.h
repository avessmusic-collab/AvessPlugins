// CHOPLINE — StretcherPool (spec §3/§6, Phase 3 warp engine)
// Preallocated Signalsmith Stretch instances for the voice pool.
//
// Budget (documented in CLAUDE.md): one DEDICATED stretcher per voice (16) so
// a stretch-warped chop can ALWAYS sound, plus a small SHARED pool (8) that
// unison streams 2–4 borrow from. When the shared pool is exhausted, extra
// unison copies simply don't sound (graceful degradation) — the musical voice
// itself is never lost. All instances are configured and warmed (one dummy
// outputSeek + process at the worst-case rate) in prepare() on the message
// thread, so the audio thread never allocates: Signalsmith's internal scratch
// vectors keep their capacity across reset()/seek()/process() once warmed.
//
// The shared checkout flags are plain bools: they are touched by the audio
// thread only (trigger/steal/voice-end all happen inside processBlock).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#if JUCE_CLANG
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif JUCE_GCC
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wshadow"
 #pragma GCC diagnostic ignored "-Wfloat-equal"
 #pragma GCC diagnostic ignored "-Wunused-parameter"
 #pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#elif JUCE_MSVC
 #pragma warning (push, 0)
#endif

#include "signalsmith-stretch/signalsmith-stretch.h"

#if JUCE_CLANG
 #pragma clang diagnostic pop
#elif JUCE_GCC
 #pragma GCC diagnostic pop
#elif JUCE_MSVC
 #pragma warning (pop)
#endif

#include <array>
#include <vector>

namespace chopline::engine
{

using Stretcher = signalsmith::stretch::SignalsmithStretch<float>;

class StretcherPool final
{
public:
    StretcherPool() = default;

    static constexpr int numDedicated = 16; // == VoicePool::numVoices
    static constexpr int numShared = 8;     // extra unison streams
    static constexpr double maxInputRate = 4.0; // Fit-step input/output clamp (spec §6)

    /// Message thread (prepareToPlay). Configures and warms every instance so
    /// later audio-thread reset()/seek()/process() calls never allocate.
    /// `cheaper` = Eco quality (Phase 5): Signalsmith presetCheaper — shorter
    /// blocks, noticeably less CPU, slightly rougher sound.
    void prepare (double sampleRate, int maxBlockSize, bool cheaper = false)
    {
        const auto rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        const auto newMaxBlock = juce::jmax (16, maxBlockSize);

        // Warming 24 stretchers costs real time — skip when nothing grew
        // (pluginval / hosts call prepareToPlay repeatedly).
        if (juce::exactlyEqual (preparedRate, rate) && newMaxBlock <= maxBlock
            && cheaper == preparedCheaper)
        {
            sharedInUse.fill (false);
            return;
        }
        preparedRate = rate;
        preparedCheaper = cheaper;
        maxBlock = newMaxBlock;
        maxSeekLength = 0;

        auto warm = [this, rate, cheaper] (Stretcher& st)
        {
            // presetDefault/presetCheaper block/interval, splitComputation=true
            // to spread the FFT work across process() calls (fewer CPU spikes).
            if (cheaper)
                st.presetCheaper (2, static_cast<float> (rate), true);
            else
                st.presetDefault (2, static_cast<float> (rate), true);

            const auto seekLen = st.outputSeekLength (static_cast<float> (maxInputRate)) + 8;
            std::vector<float> zeros (static_cast<size_t> (seekLen), 0.0f);
            const float* ins[2] = { zeros.data(), zeros.data() };

            std::vector<float> outL (static_cast<size_t> (maxBlock), 0.0f), outR (outL);
            float* outs[2] = { outL.data(), outR.data() };

            // Exercise every audio-thread path once: outputSeek (reset + seek +
            // pre-roll process), a worst-case-rate process, and a plain seek.
            st.outputSeek (ins, seekLen);
            st.process (ins, juce::jmin (seekLen, static_cast<int> (maxInputRate * maxBlock)),
                        outs, maxBlock);
            st.seek (ins, juce::jmin (seekLen, st.seekLength()), maxInputRate);
            st.reset();

            maxSeekLength = juce::jmax (maxSeekLength, seekLen);
        };

        for (auto& st : dedicatedStretchers)
            warm (st);
        for (auto& st : sharedStretchers)
            warm (st);
        sharedInUse.fill (false);
    }

    /// Scratch sizing for voices: the largest number of input samples any
    /// single stretcher call can consume (outputSeek, or one block at the
    /// worst-case Fit rate).
    int maxInputScratchSamples() const noexcept
    {
        return juce::jmax (maxSeekLength,
                           static_cast<int> (maxInputRate * maxBlock) + 16);
    }

    int maxBlockSize() const noexcept { return maxBlock; }

    // ---- audio thread -------------------------------------------------------
    Stretcher* dedicated (int voiceIndex) noexcept
    {
        return &dedicatedStretchers[static_cast<size_t> (
            juce::jlimit (0, numDedicated - 1, voiceIndex))];
    }

    Stretcher* takeShared() noexcept
    {
        for (int i = 0; i < numShared; ++i)
        {
            if (! sharedInUse[static_cast<size_t> (i)])
            {
                sharedInUse[static_cast<size_t> (i)] = true;
                return &sharedStretchers[static_cast<size_t> (i)];
            }
        }
        return nullptr; // pool exhausted: the extra unison copy stays silent
    }

    void returnShared (Stretcher* st) noexcept
    {
        if (st == nullptr)
            return;
        for (int i = 0; i < numShared; ++i)
            if (st == &sharedStretchers[static_cast<size_t> (i)])
                sharedInUse[static_cast<size_t> (i)] = false;
    }

private:
    std::array<Stretcher, numDedicated> dedicatedStretchers;
    std::array<Stretcher, numShared> sharedStretchers;
    std::array<bool, numShared> sharedInUse {};
    double preparedRate = 0.0;
    bool preparedCheaper = false;
    int maxBlock = 512;
    int maxSeekLength = 0;

    JUCE_DECLARE_NON_COPYABLE (StretcherPool)
};

} // namespace chopline::engine
