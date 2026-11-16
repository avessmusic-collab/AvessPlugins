#include "Backend/UiModel.h"
#include "PluginIdentity.h"

namespace warpr
{
    namespace
    {
        const juce::Identifier kVersion    { "warprStateVersion" };
        const juce::Identifier kLfoCount   { "lfoCount" };
        const juce::Identifier kPresetName { "presetName" };
        const juce::Identifier kIndex      { "index" };
        const juce::Identifier kId         { "id" };
        const juce::Identifier kJson       { "json" };
        const juce::Identifier kLfo        { "LFO" };
        const juce::Identifier kMatrix     { "MODMATRIX" };
        const juce::Identifier kShaper     { "SHAPER" };
        const juce::Identifier kSlice      { "SLICEMAP" };
    }

    juce::String UiModel::defaultLfoShapeJson()
    {
        // Owner request 2026-11-13: a never-edited LFO is a SINE (the UI's newShape() matches).
        // The points ride along as the CUSTOM starting shape; LfoShapeDesc::defaultShape() is the same.
        return R"({"preset":"SINE","points":[{"x":0,"v":0.1,"c":0},{"x":0.25,"v":0.9,"c":0},{"x":0.5,"v":0.35,"c":0},)"
               R"({"x":0.75,"v":0.75,"c":0},{"x":1,"v":0.1,"c":0}],"bends":[],"table":null,"drift":null})";
    }

    juce::String UiModel::defaultModMatrixJson()
    {
        return "[]";
    }

    juce::String UiModel::defaultShaperCurveJson()
    {
        // parameter-spec: default = identity (-1,-1),(0,0),(1,1), smooth interpolation.
        return R"({"interp":"smooth","points":[{"x":-1,"y":-1},{"x":0,"y":0},{"x":1,"y":1}]})";
    }

    int UiModel::curveIndex (const juce::String& curveId) noexcept
    {
        if (curveId == "A") return 0;
        if (curveId == "B") return 1;
        return -1;
    }

    juce::ValueTree UiModel::toValueTree() const
    {
        juce::ValueTree tree (identity::uiStateTag);
        tree.setProperty (kVersion, identity::uiStateVersion, nullptr);
        tree.setProperty (kLfoCount, lfoCount, nullptr);
        tree.setProperty (kPresetName, presetName, nullptr);

        for (int i = 0; i < maxLfos; ++i)
        {
            const auto& json = lfoShapeJson[(size_t) i];
            if (json.isEmpty())
                continue;

            juce::ValueTree child (kLfo);
            child.setProperty (kIndex, i, nullptr);
            child.setProperty (kJson, json, nullptr);
            tree.appendChild (child, nullptr);
        }

        if (modMatrixJson.isNotEmpty())
        {
            juce::ValueTree child (kMatrix);
            child.setProperty (kJson, modMatrixJson, nullptr);
            tree.appendChild (child, nullptr);
        }

        if (sliceMapJson.isNotEmpty())
        {
            juce::ValueTree child (kSlice);
            child.setProperty (kJson, sliceMapJson, nullptr);
            tree.appendChild (child, nullptr);
        }

        static constexpr const char* curveIds[numCurves] = { "A", "B" };
        for (int i = 0; i < numCurves; ++i)
        {
            const auto& json = shaperCurveJson[(size_t) i];
            if (json.isEmpty())
                continue;

            juce::ValueTree child (kShaper);
            child.setProperty (kId, juce::String (curveIds[i]), nullptr);
            child.setProperty (kJson, json, nullptr);
            tree.appendChild (child, nullptr);
        }

        return tree;
    }

    void UiModel::fromValueTree (const juce::ValueTree& tree)
    {
        *this = UiModel {};                                   // defaults first

        if (! tree.isValid() || tree.getType() != juce::Identifier (identity::uiStateTag))
            return;

        // A version newer than ours is read best-effort (only the fields we know).
        lfoCount = juce::jlimit (1, maxLfos, (int) tree.getProperty (kLfoCount, 3));

        if (tree.hasProperty (kPresetName))
            presetName = tree.getProperty (kPresetName).toString();

        for (int c = 0; c < tree.getNumChildren(); ++c)
        {
            const auto child = tree.getChild (c);

            if (child.hasType (kLfo))
            {
                const int index = (int) child.getProperty (kIndex, -1);
                if (index >= 0 && index < maxLfos)
                    lfoShapeJson[(size_t) index] = child.getProperty (kJson).toString();
            }
            else if (child.hasType (kMatrix))
            {
                modMatrixJson = child.getProperty (kJson).toString();
            }
            else if (child.hasType (kSlice))
            {
                sliceMapJson = child.getProperty (kJson).toString();
            }
            else if (child.hasType (kShaper))
            {
                const int index = curveIndex (child.getProperty (kId).toString());
                if (index >= 0)
                    shaperCurveJson[(size_t) index] = child.getProperty (kJson).toString();
            }
        }
    }
}
