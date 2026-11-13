#pragma once
/*
  PresetManager - preset files and the preset list (architecture C17, phase D9).

  A preset is the state container (StateIO.h) in a file with the product's preset extension
  (identity::presetExtension, ".void" for VOID).  The user directory is
      <userApplicationData>/<identity::companyDirName>/<identity::productName>/<identity::presetDirName>
  (macOS: ~/Library/VST Factory/VOID/Presets, Windows: %APPDATA%\VST Factory\VOID\Presets).

  The list shown to / stepped by the user is: factory presets first (in registration order), then the user
  files sorted by name (case-insensitive, natural order).  Factory presets are in-memory containers
  (later: BinaryData); the built-in list is EMPTY in 1.0-D9 - the infrastructure is exercised with injected
  lists in the tests.

  Everything here is plain file / memory handling on the MESSAGE thread; applying a preset is the caller's
  business (UiBackendImpl feeds the bytes to the processor's state loader).
*/

#include <juce_core/juce_core.h>

#include <vector>

namespace warpr
{
    struct FactoryPreset
    {
        juce::String name;
        juce::MemoryBlock data;                 // a complete state container
    };

    struct PresetEntry
    {
        juce::String name;                      // display name (file stem for user presets)
        bool factory = false;
        int factoryIndex = -1;                  // index into the factory list when factory
        juce::File file;                        // the user file when !factory
    };

    class PresetManager
    {
    public:
        /** `dir` is the user preset directory (created lazily on the first save). */
        explicit PresetManager (juce::File dir, std::vector<FactoryPreset> factoryPresets = builtInFactoryPresets());

        /** <userApplicationData>/<company>/<product>/<presets dir> from PluginIdentity. */
        static juce::File defaultDirectory();

        /** The factory list shipped with the plugin (empty in D9). */
        static std::vector<FactoryPreset> builtInFactoryPresets();

        const juce::File& getDirectory() const noexcept       { return directory; }
        void setDirectory (juce::File newDirectory)           { directory = std::move (newDirectory); }

        /** "." / path separators / illegal characters removed, trimmed, never empty ("Untitled"). */
        static juce::String sanitiseName (const juce::String& name);

        /** <directory>/<sanitised name><extension>. */
        juce::File fileForName (const juce::String& name) const;

        /** Factory presets, then user files sorted by name.  Missing directory -> factory only. */
        std::vector<PresetEntry> listEntries() const;

        /** The entry `delta` steps (+1 / -1, wrapping) away from the entry called `currentName`.
            Unknown current name: +1 -> first entry, -1 -> last.  Returns false when the list is empty. */
        bool stepFrom (const juce::String& currentName, int delta, PresetEntry& result) const;

        /** Reads the container bytes of an entry (size-capped).  `error` is filled on failure. */
        bool read (const PresetEntry& entry, juce::MemoryBlock& data, juce::String& error) const;
        static bool readFile (const juce::File& file, juce::MemoryBlock& data, juce::String& error);

        /** Atomic write (temporary file in the same directory, then replace); creates the directory. */
        static bool writeFile (const juce::File& file, const juce::MemoryBlock& data, juce::String& error);

        /** The preset file extension including the dot (identity::presetExtension). */
        static juce::String extension();

    private:
        juce::File directory;
        std::vector<FactoryPreset> factory;
    };
}
