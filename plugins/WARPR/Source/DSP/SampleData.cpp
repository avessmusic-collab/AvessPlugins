#include "DSP/SampleData.h"

#include "DSP/MipBuilder.h"

#include <atomic>
#include <cmath>

namespace warpr
{
    namespace
    {
        void fillGuards (SampleData::Level& lv, int channels, bool wrap)
        {
            const int n = lv.numFrames;

            for (int c = 0; c < channels; ++c)
            {
                float* d = lv.channel (c);

                for (int k = 1; k <= SampleData::pad; ++k)
                {
                    d[-k]        = wrap ? d[(n - (k % n)) % n] : 0.0f;       // frame (-k) mod n
                    d[n + k - 1] = wrap ? d[(k - 1) % n] : 0.0f;             // frame (n + k - 1) mod n
                }
            }
        }
    }

    SampleData::Ptr SampleData::create (const juce::AudioBuffer<float>& source, double sampleRate,
                                        const juce::String& sampleName, bool periodic, bool defaultWave)
    {
        const int frames = source.getNumSamples();
        const int chans  = juce::jmin (maxChannels, source.getNumChannels());

        if (frames <= 0 || chans <= 0 || ! (sampleRate > 0.0) || ! std::isfinite (sampleRate))
            return nullptr;

        static std::atomic<juce::uint64> nextSerial { 1 };

        Ptr sd (new SampleData());
        sd->serial = nextSerial.fetch_add (1, std::memory_order_relaxed);
        sd->name = sampleName;
        sd->fileSampleRate = sampleRate;
        sd->numChannels = chans;
        sd->numFrames = frames;
        sd->isDefaultWave = defaultWave;
        sd->singleCycle = periodic;

        // ---- level 0 (scrubbed copy)
        auto& l0 = sd->levels[0];
        l0.numFrames = frames;
        l0.stride = frames + 2 * pad;
        l0.storage.assign ((size_t) chans * (size_t) l0.stride, 0.0f);

        for (int c = 0; c < chans; ++c)
        {
            const float* src = source.getReadPointer (c);
            float* dst = l0.channel (c);

            for (int i = 0; i < frames; ++i)
                dst[i] = std::isfinite (src[i]) ? src[i] : 0.0f;
        }

        fillGuards (l0, chans, periodic);

        // ---- mip pyramid
        int count = 1;

        while (count < maxLevels)
        {
            const auto& prev = sd->levels[(size_t) (count - 1)];
            const int n = prev.numFrames;

            if (periodic && (n % 2) != 0)
                break;

            const int outN = periodic ? n / 2 : (n + 1) / 2;

            if (outN < (periodic ? 8 : 64))
                break;

            auto& lv = sd->levels[(size_t) count];
            lv.numFrames = outN;
            lv.stride = outN + 2 * pad;
            lv.storage.assign ((size_t) chans * (size_t) lv.stride, 0.0f);

            for (int c = 0; c < chans; ++c)
                mip::decimateByTwo (prev.channel (c), n, periodic, lv.channel (c), outN);

            fillGuards (lv, chans, periodic);
            ++count;
        }

        sd->numLevels = count;

        // ---- peaks (mono mix)
        const int pairs = juce::jmin (maxPeakPairs, frames);
        sd->peaksMinMax.resize ((size_t) pairs * 2);

        const float* c0 = l0.channel (0);
        const float* c1 = chans > 1 ? l0.channel (1) : nullptr;

        for (int b = 0; b < pairs; ++b)
        {
            const int i0 = (int) (((juce::int64) b * frames) / pairs);
            const int i1 = juce::jmax (i0 + 1, (int) (((juce::int64) (b + 1) * frames) / pairs));
            float mn = 1.0e9f, mx = -1.0e9f;

            for (int i = i0; i < i1 && i < frames; ++i)
            {
                const float v = c1 != nullptr ? 0.5f * (c0[i] + c1[i]) : c0[i];
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
            }

            sd->peaksMinMax[(size_t) b * 2]     = mn;
            sd->peaksMinMax[(size_t) b * 2 + 1] = mx;
        }

        return sd;
    }

    SampleData::Ptr SampleData::createDefaultWave()
    {
        constexpr int n = defaultWaveFrames;
        constexpr int harmonics = 64;
        const double twoPi = 6.283185307179586;

        std::vector<double> wave ((size_t) n, 0.0);
        double peak = 0.0;

        for (int i = 0; i < n; ++i)
        {
            double acc = 0.0;

            for (int h = 1; h <= harmonics; ++h)
                acc += std::sin (twoPi * (double) ((h * i) % n) / (double) n) / (double) h;     // exact periodicity

            wave[(size_t) i] = acc;
            peak = std::max (peak, std::abs (acc));
        }

        juce::AudioBuffer<float> buffer (1, n);
        const double scale = peak > 0.0 ? 0.9 / peak : 1.0;

        for (int i = 0; i < n; ++i)
            buffer.setSample (0, i, (float) (wave[(size_t) i] * scale));

        return create (buffer, (double) n * juce::MidiMessage::getMidiNoteInHertz (60),
                       "Default Wave", true, true);
    }
}
