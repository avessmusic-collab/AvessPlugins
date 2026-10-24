// CHOPLINE — Parameters (spec §3, §10)
// APVTS layout, parameter IDs and ranges. Stable IDs with a version number
// from day one; schema migrations are unit-tested against every previous
// version (spec §10).
//
// Phase 1: the only host-automatable parameter is the MIDI base note — the
// slicing controls (mode, sensitivity, min length, zero-cross snap) are
// slice-map STATE, not parameters (locked v17 design, scope: slice_map), and
// live in the ValueTree next to the sample reference.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace chopline::params
{

/// Bumped whenever the state schema changes; migrations keyed off this.
inline constexpr int kSchemaVersion = 1;

/// Parameter ID version suffix (juce::ParameterID version hint).
inline constexpr int kParameterVersion = 1;

namespace id
{
    inline constexpr auto midiBaseNote = "midiBaseNote"; // choice: C0 C1 C2 C3
}

/// Choice index (0..3) → MIDI note number (C0=24 … C3=60, Ableton naming).
inline constexpr int baseNoteForChoice (int choiceIndex) noexcept
{
    return 24 + 12 * (choiceIndex < 0 ? 0 : (choiceIndex > 3 ? 3 : choiceIndex));
}

/// Builds the full APVTS parameter layout.
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace chopline::params
