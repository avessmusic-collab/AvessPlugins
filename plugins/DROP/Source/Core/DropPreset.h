#pragma once

/**
 * DropPreset - the 16 factory presets.
 *
 * See plugins/DROP/.ideas/architecture.md > System Architecture >
 * Data-Driven Style System > Presets: "the 16 presets ARE the 16
 * DropStyles at their recommended macro defaults, not a separate data
 * structure, avoiding duplication between 'preset list' and 'style list'."
 *
 * Each entry bundles a `style` selection + sensible default values for
 * Intensity/Tension/Chaos/Impact/buildLength/dryWet/output. The 10 module
 * macros are NOT part of this table - they are always reset to 100% (the
 * "resolved open question" default - architecture.md's "Resolved Open
 * Questions" #2) by DROPAudioProcessor::setCurrentProgram(), so a loaded
 * preset always hears its Style exactly as authored, not a pre-attenuated
 * version of it.
 *
 * `static const` table, pure data, no allocation, RT-safe (though
 * setCurrentProgram() itself is message-thread-only, like all JUCE
 * AudioProcessor program-management calls).
 */
namespace drop
{

struct PresetDefaults
{
    const char* name = "";
    int styleIndex = 0;              // 0..15, matches DropStyle's table 1:1
    int buildLengthChoiceIndex = 2;  // 0..5 -> 1/2/4/8/16/32 bars (32 appended 2026-09-17)
    float intensity = 50.0f;         // 0-100%
    float tension = 50.0f;           // 0-100%
    float chaos = 0.0f;              // 0-100%
    float impact = 50.0f;            // 0-100%
    float dryWet = 50.0f;            // 0-100% (struct default 100 -> 50, POST-1.0 2026-09-18, in lockstep with the APVTS default and the re-authored preset table)
    float outputDb = 0.0f;           // -60..12 dB
};

constexpr int kNumPresets = 16;

/** Clamped 0..kNumPresets-1. */
const PresetDefaults& getPreset(int index) noexcept;

} // namespace drop
