// CHOPLINE — PresetManager (Phase 5, spec §10)
// XML presets in the user's documents folder:
//   ~/Documents/CHOPLINE/Presets/<Category>/<Name>.xml
// Categories: Chops, Glitch, Stutter, Tape, Granular, User.
//
// A preset is the SAME tree shape as the plugin state (APVTS root +
// SAMPLE/SLICES/PATTERNS children), so loading one goes through the exact
// glitch-safe path as a session restore. Two flavours (spec §10):
//   FULL     — everything, including the embedded FLAC sample.
//   KIT ONLY — patterns + chops + settings, NO sample: the SAMPLE child keeps
//              only the slicing BPM; loading onto the current sample keeps it
//              and maps the kit's markers/settings (Engine::restoreFrom
//              documents the mapping rule).
//
// NO factory content ships in this phase — the user supplies samples and
// tunes the 30 factory presets later (spec §10). Message thread only; file
// IO only (no GUI module).

#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <vector>

namespace chopline::state
{

class PresetManager final
{
public:
    struct Info
    {
        juce::String category, name;
        juce::File file;
    };

    /// `rootDir` override is for tests; the default is the documents folder.
    explicit PresetManager (juce::File rootDir = defaultRoot());

    static juce::File defaultRoot();
    static juce::StringArray factoryCategories(); // Chops … Granular + User

    const juce::File& getRoot() const noexcept { return root; }

    /// Rescans <root>/<Category>/*.xml (categories = the fixed set plus any
    /// other directory found). Sorted by category, then name.
    void rescan();
    const std::vector<Info>& getPresets() const noexcept { return presets; }
    int indexOfFile (const juce::File& f) const noexcept;

    /// Saves `fullState` (the getStateInformation tree) as a preset. Kit-only
    /// strips the embedded audio + file reference. Returns the written file.
    juce::File save (const juce::ValueTree& fullState, const juce::String& name,
                     const juce::String& category, bool kitOnly, juce::String& error);

    /// Loads a preset file; invalid tree when the file cannot be parsed.
    juce::ValueTree load (const juce::File& file) const;

    /// Kit-only transform: strips the sample audio/reference (public for tests).
    static juce::ValueTree stripSample (juce::ValueTree fullState);

private:
    juce::File root;
    std::vector<Info> presets;
};

} // namespace chopline::state
