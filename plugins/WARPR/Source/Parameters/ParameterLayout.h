#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace warpr
{
    /** Builds the APVTS layout for all 135 parameters in the exact order of ParamTable.h
        (= parameter-spec.md).  Every parameter uses juce::ParameterID { id, 1 }.  The 20
        lfoNGridX/Y parameters are registered with withAutomatable (false). */
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
}
