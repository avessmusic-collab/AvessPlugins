#pragma once

#include <functional>

#include <juce_audio_processors/juce_audio_processors.h>

namespace kickr
{
    /**
        Stage 3 Phase 3.3 — factory + user presets, Randomize / Mutate.

        Message-thread only. All param changes go through the APVTS with a single
        `UndoManager` transaction so preset-load / Randomize / Mutate are one undo step
        each. `currentSampleName` is a `state` property (not a param); a preset load /
        save round-trips it via the callback, but Randomize / Mutate NEVER touch it.

        - 47 factory patches (17 + 30 added 2026-09-02; a code table of ID->value overrides on top of the APVTS
          defaults), all synth-only (`synthEnable` on, `sampleEnable` off).
        - User presets: `<userMusicDirectory>/KICKR/Presets/<name>.kickrpreset` (XML).
          NOT `~/Library/Audio/Presets` — root-owned on many machines (same rationale
          as SampleLibrary); falls back to a temp dir if the parent is unwritable.
        - Randomize / Mutate exclusion list (architecture "Non-Automatable UI Actions"):
          oversampling, limiter, output, mix, tuneMode, tune, fineTune, velSensitivity,
          macro*, synthEnable, sampleEnable, every sample* param, currentSampleName.
    */
    class PresetManager
    {
    public:
        PresetManager (juce::AudioProcessorValueTreeState& stateToManage,
                       juce::UndoManager&                   undoToUse,
                       std::function<void (const juce::String&)> reloadSampleByName,
                       std::function<juce::String()>            currentSampleName);

        //====================================================================== factory
        static int             getNumFactory() noexcept;
        juce::StringArray      getFactoryNames() const;
        void                   loadFactory (int index);

        //======================================================================== user
        static juce::File      userFolder();
        /** Tests only — redirect the user-preset folder to a throwaway dir. */
        static void            setUserFolderForTests (const juce::File&);
        juce::StringArray      getUserNames() const;
        bool                   saveUser (const juce::String& name);
        bool                   loadUser (const juce::String& name);   // false = missing/corrupt

        /** Load a factory or user preset by display name. Returns false if not found. */
        bool                   loadByName (const juce::String& name);

        //================================================================ randomize/mutate
        void                   randomize();
        void                   mutate();

        //===================================================================== bookkeeping
        juce::String           getCurrentPresetName() const;
        void                   markDefaultIfUnnamed();

    private:
        static bool  isExcludedFromRandom (const juce::String& id) noexcept;
        void         setPresetName (const juce::String& name, const juce::File& path = {});

        /** A flushed, editable copy of the current APVTS state (params in denormalised
            units, plus the custom `current*` properties). */
        juce::ValueTree snapshotState() const;
        float           readTreeParam  (const juce::ValueTree&, juce::StringRef id) const;
        void            writeTreeParam (juce::ValueTree&, juce::StringRef id, float denorm) const;
        float           defaultOf      (juce::StringRef id) const;

        /** Push an undoable swap current-state -> `newState` (one undo step). */
        void applyStateUndoable (juce::ValueTree newState, const juce::String& transactionName);

        juce::AudioProcessorValueTreeState&       apvts;
        juce::UndoManager&                        undoManager;
        std::function<void (const juce::String&)> reloadSample;
        std::function<juce::String()>            getSampleName;
        juce::Random                             rng;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
    };
}
