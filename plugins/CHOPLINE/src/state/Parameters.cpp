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

    return layout;
}

} // namespace chopline::params
