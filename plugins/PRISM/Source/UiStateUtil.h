#pragma once

#include <juce_core/juce_core.h>

// Shared helpers + limits for the serialized UI state (apvts.state properties; ids live in Parameters/ParameterIDs.h).
// ONE definition used by PluginProcessor.cpp (state sanitiser) and PluginEditor.cpp (page bridge) so a rename or limit
// change can never silently desynchronise persistence.
namespace prism::uistate
{
    inline constexpr int kMaxNameLength = 10;                                   // source name (src1_name / src2_name)
    inline constexpr int kDefaultWidth = 1100, kDefaultHeight = 720;            // 55:36
    inline constexpr int kMinWidth = 770, kMinHeight = 504, kMaxWidth = 8192;

    inline juce::Array<int> parseInts(const juce::String& csv)                  // "226,70,302" -> {226,70,302}
    {
        juce::Array<int> out;
        for (auto& t : juce::StringArray::fromTokens(csv, ",", ""))
            out.add(t.getIntValue());
        return out;
    }

    inline juce::String joinInts(const juce::Array<int>& v)                     // {226,70,302} -> "226,70,302"
    {
        juce::StringArray parts;
        for (int e : v)
            parts.add(juce::String(e));
        return parts.joinIntoString(",");
    }

    inline juce::Array<int> intsFromVar(const juce::var& arr)                   // JS array -> ints
    {
        juce::Array<int> out;
        if (auto* a = arr.getArray())
            for (auto& e : *a)
                out.add((int) e);
        return out;
    }

    inline juce::var intsToVar(const juce::Array<int>& v)                       // ints -> JS array
    {
        juce::Array<juce::var> out;
        for (int e : v)
            out.add(e);
        return out;
    }

    // Shared source-name sanitizer: trim, cap at kMaxNameLength, trim again; empty -> fallback.
    inline juce::String sanitizeName (const juce::var& v, const juce::String& fallback)
    {
        auto s = v.toString().trim().substring (0, kMaxNameLength).trim();
        return s.isEmpty() ? fallback : s;
    }
} // namespace prism::uistate
