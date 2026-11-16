#pragma once
/*
  UiModel - the message-thread model of WARPR's NON-PARAMETER state
  (parameter-spec.md "Non-Parameter State"): lfoCount, LFO shapes, mod matrix, shaper curves,
  preset name.  It is persisted in the plugin state as the <WARPR_UI_STATE> child of the APVTS
  tree (see StateIO).

  STAGE 1 STATUS: the shape / matrix / curve documents are stored VERBATIM as JSON strings.
  Nothing parses them yet; D4 (shaper) and D8 (LFO / matrix) add the parsers, the table
  builders and the audio-thread publication.  The JSON strings are also the exact wire format
  the UI sends and receives, so storing them verbatim round-trips them bit-exactly.
*/

#include <juce_data_structures/juce_data_structures.h>

#include <array>

namespace warpr
{
    struct UiModel
    {
        static constexpr int maxLfos   = 10;
        static constexpr int numCurves = 2;          // shaper curves A, B

        int lfoCount = 3;                                        // 1..10, default 3
        std::array<juce::String, (size_t) maxLfos> lfoShapeJson; // "" = default shape
        juce::String modMatrixJson;                              // "" = empty matrix
        std::array<juce::String, (size_t) numCurves> shaperCurveJson;   // "" = identity curve
        juce::String presetName { "Init" };
        juce::String sliceMapJson;                               // D11 SLICE MODE document (SliceMap.h wire format), "" = off

        /** Default documents (spec: identity curve, empty matrix). */
        static juce::String defaultLfoShapeJson();
        static juce::String defaultModMatrixJson();
        static juce::String defaultShaperCurveJson();

        /** Curve id "A" / "B" -> 0 / 1, anything else -> -1. */
        static int curveIndex (const juce::String& curveId) noexcept;

        /** <WARPR_UI_STATE warprStateVersion=".." lfoCount=".." presetName=".."> with
            <LFO index json/>, <MODMATRIX json/>, <SHAPER id json/> children. */
        juce::ValueTree toValueTree() const;

        /** Tolerant reader: unknown / missing / out-of-range content falls back to defaults
            and never asserts or throws.  A newer warprStateVersion loads what is understood. */
        void fromValueTree (const juce::ValueTree& tree);
    };
}
