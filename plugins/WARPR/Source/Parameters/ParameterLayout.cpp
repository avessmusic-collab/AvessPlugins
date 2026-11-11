#include "Parameters/ParameterLayout.h"
#include "Parameters/ParamTable.h"

#include <iterator>
#include <memory>

namespace
{
    using ParamPtr = std::unique_ptr<juce::RangedAudioParameter>;

    ParamPtr makeFloat (const char* id, const char* name, float mn, float mx, float step,
                        float skewCentre, float def, const char* unit)
    {
        juce::NormalisableRange<float> range (mn, mx, step);
        if (skewCentre > 0.0f)
            range.setSkewForCentre (skewCentre);

        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, range, def,
            juce::AudioParameterFloatAttributes().withLabel (unit));
    }

    ParamPtr makeInt (const char* id, const char* name, int mn, int mx, int def, const char* unit)
    {
        return std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { id, 1 }, name, mn, mx, def,
            juce::AudioParameterIntAttributes().withLabel (unit));
    }

    ParamPtr makeBool (const char* id, const char* name, bool def)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def);
    }

    template <std::size_t N>
    ParamPtr makeChoice (const char* id, const char* name, int def, const char* const (&list)[N], bool automatable)
    {
        juce::StringArray items;
        for (const char* s : list)
            items.add (juce::String::fromUTF8 (s));      // some entries are non-ASCII (UTF-8)

        return std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { id, 1 }, name, items, def,
            juce::AudioParameterChoiceAttributes().withAutomatable (automatable));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout warpr::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

   #define WARPR_LAYOUT_F(id, name, mn, mx, step, centre, def, unit) \
        layout.add (makeFloat (#id, name, mn, mx, step, centre, def, unit));
   #define WARPR_LAYOUT_I(id, name, mn, mx, def, unit) \
        layout.add (makeInt (#id, name, mn, mx, def, unit));
   #define WARPR_LAYOUT_B(id, name, def) \
        layout.add (makeBool (#id, name, def));
   #define WARPR_LAYOUT_C(id, name, def, lst, automatable) \
        layout.add (makeChoice (#id, name, def, ::warpr::choices::lst, automatable));

    WARPR_PARAM_TABLE (WARPR_LAYOUT_F, WARPR_LAYOUT_I, WARPR_LAYOUT_B, WARPR_LAYOUT_C)

   #undef WARPR_LAYOUT_F
   #undef WARPR_LAYOUT_I
   #undef WARPR_LAYOUT_B
   #undef WARPR_LAYOUT_C

    return layout;
}
