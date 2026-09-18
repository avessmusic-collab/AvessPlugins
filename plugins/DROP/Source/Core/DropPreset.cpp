#include "DropPreset.h"
#include <array>
#include <juce_core/juce_core.h>

namespace drop
{

namespace
{
    const std::array<PresetDefaults, (size_t) kNumPresets>& presetTable() noexcept
    {
        static const std::array<PresetDefaults, (size_t) kNumPresets> table = [] {
            std::array<PresetDefaults, (size_t) kNumPresets> t {};

            // POST-1.0 (2026-09-18, user-directed "make default dry wet
            // 50%"): every factory preset's dryWet re-authored 100 -> 50 in
            // lockstep with the APVTS default change, so selecting a preset
            // never snaps the mix back to full-wet.
            auto set = [&t](int i, const char* name, int style, int buildIdx,
                             float intensity, float tension, float chaos, float impact,
                             float dryWet, float outDb)
            {
                t[(size_t) i] = PresetDefaults { name, style, buildIdx, intensity, tension, chaos, impact, dryWet, outDb };
            };

            set(0,  "Clean EDM",       0,  2, 55.0f, 55.0f,  0.0f, 55.0f, 50.0f, 0.0f);
            set(1,  "Big Room",        1,  2, 70.0f, 60.0f, 10.0f, 70.0f, 50.0f, 0.0f);
            set(2,  "Techno Pressure", 2,  1, 65.0f, 65.0f, 15.0f, 60.0f, 50.0f, 0.0f);
            set(3,  "Dark Techno",     3,  1, 75.0f, 70.0f, 25.0f, 65.0f, 50.0f, 0.0f);
            set(4,  "Trap Pull",       4,  1, 65.0f, 60.0f, 20.0f, 60.0f, 50.0f, 0.0f);
            set(5,  "DnB Rush",        5,  0, 75.0f, 75.0f, 20.0f, 65.0f, 50.0f, 0.0f);
            set(6,  "Hyper",           6,  1, 85.0f, 80.0f, 45.0f, 80.0f, 50.0f, 0.0f);
            set(7,  "Cinematic",       7,  3, 45.0f, 45.0f,  5.0f, 45.0f, 50.0f, 0.0f);
            set(8,  "Minimal",         8,  2, 30.0f, 40.0f,  0.0f, 35.0f, 50.0f, 0.0f);
            set(9,  "Vocal Build",     9,  2, 45.0f, 50.0f,  5.0f, 45.0f, 50.0f, 0.0f);
            set(10, "Drum Build",      10, 1, 60.0f, 60.0f, 15.0f, 60.0f, 50.0f, 0.0f);
            set(11, "Synth Build",     11, 2, 65.0f, 60.0f, 20.0f, 60.0f, 50.0f, 0.0f);
            set(12, "Full Mix",        12, 2, 40.0f, 45.0f,  0.0f, 45.0f, 50.0f, 0.0f);
            set(13, "Destroy",         13, 1, 90.0f, 85.0f, 55.0f, 90.0f, 50.0f, 0.0f);
            set(14, "Vacuum",          14, 2, 60.0f, 65.0f, 10.0f, 75.0f, 50.0f, 0.0f);
            set(15, "Reverse Energy",  15, 3, 55.0f, 55.0f, 10.0f, 40.0f, 50.0f, 0.0f);

            return t;
        }();

        return table;
    }
}

const PresetDefaults& getPreset(int index) noexcept
{
    const auto& table = presetTable();
    const int idx = juce::jlimit(0, kNumPresets - 1, index);
    return table[(size_t) idx];
}

} // namespace drop
