// CHOPLINE — ChopVoice implementation. See header for the design.

#include "ChopVoice.h"

#include <cmath>

namespace chopline::engine
{

namespace
{
    // Equal-power fade gain for position k of total (k in [0, total)).
    inline float fadeGain (juce::int64 k, int total) noexcept
    {
        return std::sin (juce::MathConstants<float>::halfPi
                         * (static_cast<float> (k) + 1.0f) / static_cast<float> (total));
    }
} // namespace

void ChopVoice::prepare (double sampleRate) noexcept
{
    const auto fadeSamples = juce::jmax (1, static_cast<int> (sampleRate * fastFadeSeconds));
    fastFadeStep = 1.0f / static_cast<float> (fadeSamples);
    reset();
}

void ChopVoice::reset() noexcept
{
    main.stop();
    tail.stop();
    stamp = 0;
    uiChop.store (-1, std::memory_order_relaxed);
    uiPos.store (0.0f, std::memory_order_relaxed);
}

void ChopVoice::copyStream (Stream& dst, const Stream& src) noexcept
{
    dst.sample.store (src.sample.load (std::memory_order_relaxed), std::memory_order_relaxed);
    dst.pos = src.pos;
    dst.start = src.start;
    dst.end = src.end;
    dst.fadeIn = src.fadeIn;
    dst.fadeOut = src.fadeOut;
    dst.gain = src.gain;
    dst.fastGain = src.fastGain;
    dst.fastFading = src.fastFading;
    dst.active = src.active;
}

void ChopVoice::trigger (const SampleData* sample, const SliceMapSnapshot::Chop& chop,
                         int chopIndex, float gain, std::uint64_t newStamp) noexcept
{
    if (main.active)
    {
        // Move the running stream into the tail and fast-fade it (2 ms steal fade).
        copyStream (tail, main);
        tail.fastFading = true;
    }

    main.pos = chop.start;
    main.start = chop.start;
    main.end = chop.end;
    main.fadeIn = juce::jmax (1, chop.fadeInSamples);
    main.fadeOut = juce::jmax (1, chop.fadeOutSamples);
    main.gain = gain;
    main.fastGain = 1.0f;
    main.fastFading = false;
    main.active = true;
    main.sample.store (sample, std::memory_order_relaxed);

    stamp = newStamp;
    uiChop.store (chopIndex, std::memory_order_relaxed);
    uiPos.store (0.0f, std::memory_order_relaxed);
}

void ChopVoice::fastRelease() noexcept
{
    if (main.active)
        main.fastFading = true;
    if (tail.active)
        tail.fastFading = true;
}

void ChopVoice::Stream::renderAdd (juce::AudioBuffer<float>& out, int startSample, int numSamples,
                                   float fadeStep) noexcept
{
    if (! active)
        return;

    const auto* s = sample.load (std::memory_order_relaxed);
    if (s == nullptr || s->audio.getNumSamples() <= 0)
    {
        stop();
        return;
    }

    auto* l = out.getWritePointer (0);
    auto* r = out.getNumChannels() > 1 ? out.getWritePointer (1) : nullptr;
    const auto* srcL = s->audio.getReadPointer (0);
    const auto* srcR = s->audio.getNumChannels() > 1 ? s->audio.getReadPointer (1) : srcL;

    const auto sampleEnd = static_cast<juce::int64> (s->audio.getNumSamples());

    for (int i = startSample; i < startSample + numSamples; ++i)
    {
        if (pos >= end || pos >= sampleEnd)
        {
            stop();
            return;
        }

        auto g = gain;
        const auto played = pos - start;
        const auto remaining = end - pos;
        if (played < fadeIn)
            g *= fadeGain (played, fadeIn);
        if (remaining <= fadeOut)
            g *= fadeGain (remaining - 1, fadeOut);
        if (fastFading)
        {
            g *= fastGain;
            fastGain -= fadeStep;
            if (fastGain <= 0.0f)
            {
                stop();
                return;
            }
        }

        l[i] += srcL[pos] * g;
        if (r != nullptr)
            r[i] += srcR[pos] * g;
        ++pos;
    }
}

void ChopVoice::renderAdd (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept
{
    if (! isActive() || numSamples <= 0)
        return;

    main.renderAdd (out, startSample, numSamples, fastFadeStep);
    tail.renderAdd (out, startSample, numSamples, fastFadeStep);

    // UI feedback (polled by the editor; plain word-sized stores).
    if (main.active)
    {
        const auto len = juce::jmax<juce::int64> (1, main.end - main.start);
        uiPos.store (static_cast<float> (main.pos - main.start) / static_cast<float> (len),
                     std::memory_order_relaxed);
    }
    else if (! tail.active)
    {
        uiChop.store (-1, std::memory_order_relaxed);
        uiPos.store (0.0f, std::memory_order_relaxed);
    }
}

} // namespace chopline::engine
