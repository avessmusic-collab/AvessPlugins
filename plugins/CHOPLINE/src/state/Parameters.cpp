// CHOPLINE — Parameters implementation.

#include "Parameters.h"

namespace chopline::params
{

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { id::midiBaseNote, kParameterVersion },
        "MIDI Base Note",
        juce::StringArray { "C0", "C1", "C2", "C3" },
        1)); // default C1 = MIDI note 36 (chop 1)

    // Phase 2 (spec §5): swing 0–75 % on even steps — trigger TIME only, so a
    // per-block atomic read is enough (no audio-rate smoothing needed).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { id::swing, kParameterVersion },
        "Swing",
        juce::NormalisableRange<float> (0.0f, 75.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    return layout;
}

} // namespace chopline::params
