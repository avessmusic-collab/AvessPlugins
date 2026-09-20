// DROP_Profiles - offline rise/fade profile renderer (POST-1.0 dev tool,
// 2026-09-20). Renders REAL processBlock() audio (no mockups) for each
// requested configuration and prints windowed-RMS trajectories as JSON:
//   - all 16 factory presets (their own authored macros; buildLength forced
//     to 1 bar for comparability), noiseType WHITE, tail CUT and 0.70s
//   - all 7 noiseType choices on the Clean EDM preset, tail CUT and 0.70s
// Input is SILENT throughout, so every curve is DROP's OWN sound: the riser
// rising through the build (plus its delay/reverb wash) and the tail ring
// fading after the drop. A host-style playhead (120 BPM, starting exactly
// on a bar) keeps Arming at ~0 so renders stay compact.

#include "PluginProcessor.h"
#include "Core/DropPreset.h"
#include <cstdio>
#include <vector>
#include <cmath>

namespace
{
    struct TestPlayHead : public juce::AudioPlayHead
    {
        juce::AudioPlayHead::PositionInfo info;
        juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override { return info; }
    };

    constexpr double kSampleRate = 48000.0;
    constexpr int kBlockSize = 512;
    constexpr double kBpm = 120.0;
    constexpr double kSamplesPerQuarter = kSampleRate * 60.0 / kBpm; // 24000
    constexpr int kRmsWindow = 1024; // ~21ms per point

    struct Profile
    {
        juce::String name;
        bool tailOn = false;
        std::vector<float> rms;      // one point per kRmsWindow samples, from trigger
        int64_t impactSample = -1;   // sample index (same timeline as rms points) where Impact was first observed
        int64_t lastAudible = -1;
    };

    Profile render (const juce::String& name, int presetIndex, int noiseTypeIndex, bool tailOn)
    {
        Profile prof;
        prof.name = name;
        prof.tailOn = tailOn;

        DROPAudioProcessor p;
        p.setRateAndBufferSizeDetails (kSampleRate, kBlockSize);
        p.prepareToPlay (kSampleRate, kBlockSize);
        TestPlayHead ph;
        p.setPlayHead (&ph);

        auto setParam = [&] (const char* id, float normalized)
        {
            if (auto* prm = p.parameters.getParameter (id)) prm->setValueNotifyingHost (normalized);
        };

        // Preset values (the same fields the preset selector applies).
        const auto& preset = drop::getPreset (presetIndex);
        setParam ("style", (float) preset.styleIndex / 15.0f);
        setParam ("intensity", preset.intensity / 100.0f);
        setParam ("tension", preset.tension / 100.0f);
        setParam ("chaos", preset.chaos / 100.0f);
        setParam ("impact", preset.impact / 100.0f);
        setParam ("dryWet", 1.0f); // fully wet: the profile shows DROP's own sound, unattenuated
        setParam ("buildLength", 0.0f); // 1 bar, comparability across presets
        setParam ("noiseType", (float) noiseTypeIndex / 6.0f);
        setParam ("tail", tailOn ? 0.4691f : 0.0f); // 0.70s default vs CUT
        setParam ("trigger", 1.0f);

        const int totalBlocks = (int) ((14.0 * kSampleRate) / kBlockSize); // covers 2s build + impact + up to ~10s max ring + margin
        double sumSq = 0.0;
        int samplesInWindow = 0;
        int64_t samplePos = 0;
        bool triggered = false;

        for (int block = 0; block < totalBlocks; ++block)
        {
            const double ppq = (double) samplePos / kSamplesPerQuarter;
            ph.info = juce::AudioPlayHead::PositionInfo {};
            ph.info.setBpm (kBpm);
            juce::AudioPlayHead::TimeSignature sig; sig.numerator = 4; sig.denominator = 4;
            ph.info.setTimeSignature (sig);
            ph.info.setPpqPosition (ppq);
            ph.info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            ph.info.setIsPlaying (true);

            juce::AudioBuffer<float> buf (2, kBlockSize);
            buf.clear();
            juce::MidiBuffer midi;
            p.processBlock (buf, midi);
            if (! triggered) { setParam ("trigger", 0.0f); triggered = true; }

            if (prof.impactSample < 0 && p.getCurrentDropState() == drop::DropState::Impact)
                prof.impactSample = samplePos;

            const float* out = buf.getReadPointer (0);
            for (int i = 0; i < kBlockSize; ++i)
            {
                sumSq += (double) out[i] * (double) out[i];
                if (++samplesInWindow == kRmsWindow)
                {
                    const float rms = (float) std::sqrt (sumSq / (double) kRmsWindow);
                    prof.rms.push_back (rms);
                    if (rms > 1.0e-4f) prof.lastAudible = samplePos + i;
                    sumSq = 0.0;
                    samplesInWindow = 0;
                }
            }
            samplePos += kBlockSize;

            // Early out: past Impact, once we've been silent for ~1.5s the profile is over.
            if (prof.impactSample > 0 && prof.lastAudible > 0
                && samplePos - prof.lastAudible > (int64_t) (1.5 * kSampleRate))
                break;
        }
        p.releaseResources();
        return prof;
    }

    void printProfileJson (const Profile& prof, bool& first)
    {
        if (! first) std::printf (",\n");
        first = false;
        std::printf ("  {\"name\": \"%s\", \"tail\": %s, \"impactSample\": %lld, \"lastAudible\": %lld, \"windowSamples\": %d, \"rms\": [",
                     prof.name.toRawUTF8(), prof.tailOn ? "true" : "false",
                     (long long) prof.impactSample, (long long) prof.lastAudible, kRmsWindow);
        for (size_t i = 0; i < prof.rms.size(); ++i)
            std::printf ("%s%.6g", i > 0 ? "," : "", (double) prof.rms[i]);
        std::printf ("]}");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const char* noiseNames[] = { "STYLE", "WHITE", "PINK", "BROWN", "SWEEP", "SAW", "SHEPARD" };

    std::printf ("{\n\"presets\": [\n");
    bool first = true;
    for (int i = 0; i < drop::kNumPresets; ++i)
    {
        const auto& preset = drop::getPreset (i);
        for (bool tailOn : { false, true })
        {
            std::fprintf (stderr, "rendering preset %d/%d (%s) tail=%d...\n", i + 1, drop::kNumPresets, preset.name, tailOn ? 1 : 0);
            const auto prof = render (preset.name, i, 1 /* WHITE */, tailOn);
            printProfileJson (prof, first);
        }
    }
    std::printf ("\n],\n\"noiseTypes\": [\n");
    first = true;
    for (int n = 0; n < 7; ++n)
    {
        for (bool tailOn : { false, true })
        {
            std::fprintf (stderr, "rendering noiseType %s tail=%d...\n", noiseNames[n], tailOn ? 1 : 0);
            const auto prof = render (noiseNames[n], 0 /* Clean EDM */, n, tailOn);
            printProfileJson (prof, first);
        }
    }
    std::printf ("\n]\n}\n");
    return 0;
}
