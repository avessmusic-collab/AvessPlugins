// CHOPLINE — Parameters (spec §3, §10)
// APVTS layout, parameter IDs and ranges. Stable IDs with a version number
// from day one; schema migrations are unit-tested against every previous
// version (spec §10).
//
// Phase 0: the layout is intentionally EMPTY — ready to fill from Phase 1.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace chopline::params
{

/// Bumped whenever the state schema changes; migrations keyed off this.
inline constexpr int kSchemaVersion = 1;

/// Parameter ID version suffix (juce::ParameterID version hint).
inline constexpr int kParameterVersion = 1;

/// Builds the full APVTS parameter layout. Empty in Phase 0.
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace chopline::params
