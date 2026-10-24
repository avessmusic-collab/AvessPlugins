// CHOPLINE — VoicePool (spec §3)
// Fixed pool of 16 preallocated ChopVoices. A trigger takes a free voice, or
// steals the oldest one (the voice itself moves its running stream into a
// 2 ms fast-fading tail, so stealing never clicks). Everything audio-thread
// safe: no allocation, the pool is a plain std::array.

#pragma once

#include "ChopVoice.h"

#include <array>

namespace chopline::engine
{

class VoicePool final
{
public:
    static constexpr int numVoices = 16;

    void prepare (double sampleRate) noexcept
    {
        for (auto& v : voices)
            v.prepare (sampleRate);
        nextStamp = 1;
    }

    void reset() noexcept
    {
        for (auto& v : voices)
            v.reset();
    }

    // ---- audio thread -------------------------------------------------------
    void trigger (const SampleData* sample, const SliceMapSnapshot::Chop& chop,
                  int chopIndex, float gain) noexcept
    {
        ChopVoice* chosen = nullptr;
        for (auto& v : voices)
        {
            if (! v.isActive())
            {
                chosen = &v;
                break;
            }
        }
        if (chosen == nullptr) // steal the oldest (its tail declicks the takeover)
        {
            chosen = &voices[0];
            for (auto& v : voices)
                if (v.getStamp() < chosen->getStamp())
                    chosen = &v;
        }
        chosen->trigger (sample, chop, chopIndex, gain, nextStamp++);
    }

    void renderAdd (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept
    {
        for (auto& v : voices)
            v.renderAdd (out, startSample, numSamples);
    }

    void releaseAll() noexcept
    {
        for (auto& v : voices)
            v.fastRelease();
    }

    // ---- message thread -------------------------------------------------------
    int countActive() const noexcept
    {
        int n = 0;
        for (auto& v : voices)
            if (v.isActive())
                ++n;
        return n;
    }

    bool usesSample (const SampleData* s) const noexcept
    {
        for (auto& v : voices)
            if (v.usesSample (s))
                return true;
        return false;
    }

    const ChopVoice& getVoice (int index) const noexcept
    {
        return voices[static_cast<size_t> (juce::jlimit (0, numVoices - 1, index))];
    }

private:
    std::array<ChopVoice, numVoices> voices;
    std::uint64_t nextStamp = 1;
};

} // namespace chopline::engine
