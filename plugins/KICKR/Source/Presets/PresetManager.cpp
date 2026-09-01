#include "Presets/PresetManager.h"
#include "Parameters/ParameterIDs.h"

#include <array>
#include <cmath>

namespace kickr
{
    namespace
    {
        namespace pid = id;

        struct Override { const char* id; float value; };

        struct Factory
        {
            const char*                   name;
            std::initializer_list<Override> overrides;
        };

        // 17 factory patches — a small set of ID->value overrides on top of the APVTS
        // defaults. All synth-only. Tuned by ear intent; refine at repo Stage 17.
        const std::array<Factory, 17> kFactory { {
            { "Clean",       { {pid::drive,0.05f},{pid::character,0.0f},{pid::tailLevel,0.10f},
                               {pid::bodyDecay,350.0f},{pid::pitchStart,3.0f},{pid::clickLevel,0.25f} } },
            { "House",       { {pid::fundamental,50.0f},{pid::pitchStart,3.0f},{pid::pitchTime,45.0f},
                               {pid::bodyDecay,300.0f},{pid::drive,0.20f},{pid::clickLevel,0.35f},
                               {pid::clickTone,3500.0f},{pid::tailLevel,0.15f} } },
            { "Techno",      { {pid::fundamental,52.0f},{pid::pitchStart,5.0f},{pid::pitchTime,40.0f},
                               {pid::bodyDecay,250.0f},{pid::drive,0.35f},{pid::character,0.15f},
                               {pid::clickLevel,0.40f},{pid::clickTone,5000.0f},{pid::tailLevel,0.20f},
                               {pid::tailLength,180.0f} } },
            { "Hard Techno", { {pid::fundamental,55.0f},{pid::pitchStart,7.0f},{pid::pitchTime,35.0f},
                               {pid::bodyDecay,200.0f},{pid::drive,0.55f},{pid::character,0.35f},
                               {pid::transientAttack,0.40f},{pid::clickLevel,0.50f},{pid::tailDrive,0.40f},
                               {pid::tailLength,220.0f} } },
            { "Hardstyle",   { {pid::fundamental,60.0f},{pid::pitchStart,9.0f},{pid::pitchTime,30.0f},
                               {pid::bodyDecay,180.0f},{pid::drive,0.70f},{pid::character,0.50f},
                               {pid::tailLevel,0.45f},{pid::tailLength,700.0f},{pid::tailTone,0.75f},
                               {pid::tailDrive,0.70f},{pid::clickLevel,0.45f} } },
            { "Hardcore",    { {pid::fundamental,65.0f},{pid::pitchStart,8.0f},{pid::pitchTime,25.0f},
                               {pid::bodyDecay,150.0f},{pid::drive,0.85f},{pid::character,0.65f},
                               {pid::transientAttack,0.60f},{pid::tailLevel,0.30f},{pid::tailDrive,0.60f},
                               {pid::clickLevel,0.55f},{pid::low,4.0f} } },
            { "Industrial",  { {pid::fundamental,48.0f},{pid::pitchStart,4.0f},{pid::pitchTime,60.0f},
                               {pid::bodyDecay,400.0f},{pid::drive,0.60f},{pid::character,0.55f},
                               {pid::noiseLevel,0.25f},{pid::noiseType,2.0f},{pid::noiseTone,0.60f},
                               {pid::tailLength,350.0f},{pid::tailDrive,0.50f} } },
            { "Sub Heavy",   { {pid::fundamental,40.0f},{pid::pitchStart,2.5f},{pid::pitchTime,70.0f},
                               {pid::bodyLevel,1.0f},{pid::bodyDecay,600.0f},{pid::subLevel,0.80f},
                               {pid::subFreq,35.0f},{pid::subDecay,500.0f},{pid::drive,0.10f},
                               {pid::clickLevel,0.15f},{pid::tailLevel,0.10f} } },
            { "Short",       { {pid::fundamental,55.0f},{pid::pitchStart,4.0f},{pid::pitchTime,30.0f},
                               {pid::bodyDecay,90.0f},{pid::subDecay,80.0f},{pid::tailLevel,0.05f},
                               {pid::clickLevel,0.50f},{pid::clickTime,2.0f} } },
            { "Long",        { {pid::fundamental,50.0f},{pid::pitchStart,3.0f},{pid::pitchTime,80.0f},
                               {pid::bodyDecay,900.0f},{pid::subDecay,700.0f},{pid::tailLevel,0.35f},
                               {pid::tailLength,1200.0f},{pid::tailTone,0.40f} } },
            { "Distorted",   { {pid::fundamental,55.0f},{pid::pitchStart,5.0f},{pid::drive,0.80f},
                               {pid::character,0.55f},{pid::driveMix,0.85f},{pid::bodyHarmonics,0.40f},
                               {pid::transientAttack,0.30f},{pid::clickLevel,0.40f} } },
            { "Clicky",      { {pid::fundamental,55.0f},{pid::pitchStart,4.0f},{pid::bodyDecay,160.0f},
                               {pid::clickLevel,0.75f},{pid::clickTone,8000.0f},{pid::clickPitch,7000.0f},
                               {pid::clickTime,1.5f},{pid::transientAttack,0.50f},{pid::tailLevel,0.08f} } },
            { "Punchy",      { {pid::fundamental,55.0f},{pid::pitchStart,6.0f},{pid::pitchTime,38.0f},
                               {pid::pitchCurve,0.85f},{pid::bodyDecay,240.0f},{pid::transientAttack,0.55f},
                               {pid::transientSustain,-0.10f},{pid::drive,0.30f},{pid::clickLevel,0.45f} } },
            { "Warehouse",   { {pid::fundamental,52.0f},{pid::pitchStart,5.0f},{pid::pitchTime,42.0f},
                               {pid::bodyDecay,280.0f},{pid::drive,0.40f},{pid::character,0.20f},
                               {pid::clickLevel,0.40f},{pid::clickTone,4500.0f},{pid::tailLevel,0.25f},
                               {pid::tailLength,240.0f},{pid::tailTone,0.45f},{pid::low,2.0f} } },
            { "EDM",         { {pid::fundamental,48.0f},{pid::pitchStart,4.0f},{pid::pitchTime,50.0f},
                               {pid::bodyDecay,320.0f},{pid::drive,0.30f},{pid::clickLevel,0.50f},
                               {pid::clickTone,6000.0f},{pid::tailLevel,0.20f},{pid::high,3.0f},
                               {pid::transientAttack,0.40f} } },
            { "Trap",        { {pid::fundamental,42.0f},{pid::pitchStart,3.0f},{pid::pitchTime,120.0f},
                               {pid::pitchCurve,0.60f},{pid::bodyDecay,500.0f},{pid::subLevel,0.70f},
                               {pid::subFreq,38.0f},{pid::subDecay,600.0f},{pid::drive,0.25f},
                               {pid::clickLevel,0.30f},{pid::tailLevel,0.15f} } },
            { "Cinematic",   { {pid::fundamental,38.0f},{pid::pitchStart,3.0f},{pid::pitchTime,90.0f},
                               {pid::bodyLevel,1.0f},{pid::bodyDecay,1200.0f},{pid::subLevel,0.60f},
                               {pid::subDecay,900.0f},{pid::tailLevel,0.40f},{pid::tailLength,1600.0f},
                               {pid::tailTone,0.35f},{pid::tailDrive,0.30f},{pid::drive,0.15f},
                               {pid::low,3.0f} } },
        } };

        // Musically sensible sub-ranges for Randomize (NOT the full param range).
        struct RandRange { const char* id; float lo; float hi; };
        const std::array<RandRange, 33> kRandRanges { {
            { pid::fundamental,      35.0f,   70.0f },
            { pid::pitchStart,        2.0f,    8.0f },
            { pid::pitchTime,        20.0f,  120.0f },
            { pid::pitchCurve,        0.40f,   1.0f },
            { pid::bodyLevel,         0.70f,   1.0f },
            { pid::bodyDecay,       120.0f,  700.0f },
            { pid::bodyHarmonics,     0.0f,    0.40f },
            { pid::morph,             0.0f,    0.60f },   // 2026-09-01 v2: attack-only phase-skew — gentle by design, so Randomize can reach a bit further
            { pid::subLevel,          0.20f,   0.80f },
            { pid::subFreq,          30.0f,   55.0f },
            { pid::subDecay,        100.0f,  500.0f },
            { pid::clickLevel,        0.20f,   0.80f },
            { pid::clickTone,      2500.0f, 9000.0f },
            { pid::clickTime,         1.0f,   12.0f },
            { pid::clickPitch,     3000.0f, 9000.0f },
            { pid::tailLevel,         0.0f,    0.50f },
            { pid::tailLength,       80.0f,  600.0f },
            { pid::tailTone,          0.20f,   0.80f },
            { pid::tailDrive,         0.0f,    0.50f },
            { pid::noiseDecay,       30.0f,  200.0f },
            { pid::noiseTone,         0.30f,   0.80f },
            { pid::transientAttack,  -0.30f,   0.70f },
            { pid::transientSustain, -0.30f,   0.40f },
            { pid::drive,             0.10f,   0.70f },
            { pid::character,         0.0f,    0.60f },
            { pid::driveMix,          0.60f,   1.0f },
            { pid::low,              -3.0f,    6.0f },
            { pid::mid,              -4.0f,    3.0f },
            { pid::high,             -3.0f,    5.0f },
            { pid::bodyWidth,         0.0f,    0.30f },
            { pid::clickWidth,        0.20f,   0.70f },
            { pid::outputWidth,       0.40f,   0.70f },
            { pid::noiseLevel,        0.0f,    0.30f },   // handled specially (often 0)
        } };
    }

    //==========================================================================
    PresetManager::PresetManager (juce::AudioProcessorValueTreeState& stateToManage,
                                  juce::UndoManager&                  undoToUse,
                                  std::function<void (const juce::String&)> reloadSampleByName,
                                  std::function<juce::String()>            currentSampleName)
        : apvts (stateToManage),
          undoManager (undoToUse),
          reloadSample (std::move (reloadSampleByName)),
          getSampleName (std::move (currentSampleName))
    {
    }

    //====================================================================== helpers
    // Everything below works at the state-tree level: build the target tree, then do one
    // undoable `replaceState` swap. This sidesteps the APVTS param->tree flush, which is
    // otherwise driven by a Timer and so would make `beginNewTransaction` + a bare
    // `setValueNotifyingHost` un-undoable in a headless / paused-UI context.

    juce::ValueTree PresetManager::snapshotState() const
    {
        return apvts.copyState();   // flushes pending param values, then returns a copy
    }

    float PresetManager::readTreeParam (const juce::ValueTree& tree, juce::StringRef pId) const
    {
        const auto child = tree.getChildWithProperty ("id", juce::String (pId));
        return child.isValid() ? (float) child.getProperty ("value") : defaultOf (pId);
    }

    void PresetManager::writeTreeParam (juce::ValueTree& tree, juce::StringRef pId, float denorm) const
    {
        if (auto* p = apvts.getParameter (pId))
        {
            const auto& r = p->getNormalisableRange();
            denorm = juce::jlimit (r.start, r.end, r.snapToLegalValue (denorm));
        }
        auto child = tree.getChildWithProperty ("id", juce::String (pId));
        if (child.isValid())
            child.setProperty ("value", denorm, nullptr);
    }

    float PresetManager::defaultOf (juce::StringRef pId) const
    {
        if (auto* p = apvts.getParameter (pId))
            return p->convertFrom0to1 (p->getDefaultValue());
        return 0.0f;
    }

    void PresetManager::applyStateUndoable (juce::ValueTree newState, const juce::String& transactionName)
    {
        struct SwapAction final : juce::UndoableAction
        {
            SwapAction (juce::AudioProcessorValueTreeState& a,
                        juce::ValueTree before, juce::ValueTree after,
                        std::function<void (const juce::String&)> reload)
                : apvts (a), oldTree (std::move (before)), newTree (std::move (after)),
                  reloadCb (std::move (reload)) {}

            bool apply (const juce::ValueTree& t)
            {
                apvts.replaceState (t.createCopy());
                if (reloadCb)
                    reloadCb (t.getProperty ("currentSampleName", juce::String()).toString());
                return true;
            }
            bool perform() override { return apply (newTree); }
            bool undo()    override { return apply (oldTree); }
            int  getSizeInUnits() override { return 2048; }

            juce::AudioProcessorValueTreeState& apvts;
            juce::ValueTree oldTree, newTree;
            std::function<void (const juce::String&)> reloadCb;
        };

        auto before = snapshotState();
        // carry the sample name the processor currently holds into `before` so an undo
        // restores it too.
        if (getSampleName)
            before.setProperty ("currentSampleName", getSampleName(), nullptr);

        undoManager.beginNewTransaction (transactionName);
        undoManager.perform (new SwapAction (apvts, before, std::move (newState), reloadSample),
                             transactionName);
    }

    void PresetManager::setPresetName (const juce::String& name, const juce::File& path)
    {
        apvts.state.setProperty ("currentPresetName", name, nullptr);
        apvts.state.setProperty ("currentPresetPath", path.getFullPathName(), nullptr);
    }

    juce::String PresetManager::getCurrentPresetName() const
    {
        return apvts.state.getProperty ("currentPresetName", juce::String()).toString();
    }

    void PresetManager::markDefaultIfUnnamed()
    {
        if (getCurrentPresetName().isEmpty())
            setPresetName ("Default");
    }

    bool PresetManager::isExcludedFromRandom (const juce::String& pId) noexcept
    {
        static const juce::StringArray excluded {
            pid::oversampling, pid::limiter, pid::output, pid::mix,
            pid::tuneMode, pid::tune, pid::fineTune, pid::velSensitivity,
            pid::macroPunch, pid::macroBody, pid::macroCrush, pid::macroTail,
            pid::synthEnable, pid::sampleEnable,
            pid::sampleLevel, pid::sampleStart, pid::sampleEnd, pid::sampleReverse,
            pid::sampleTune, pid::sampleFine, pid::sampleMidiTrack, pid::sampleAttack,
            pid::sampleDecay, pid::sampleHP, pid::sampleLP, pid::sampleCrush
        };
        return excluded.contains (pId);
    }

    //====================================================================== factory
    int PresetManager::getNumFactory() noexcept { return (int) kFactory.size(); }

    juce::StringArray PresetManager::getFactoryNames() const
    {
        juce::StringArray names;
        for (const auto& f : kFactory)
            names.add (f.name);
        return names;
    }

    void PresetManager::loadFactory (int index)
    {
        if (index < 0 || index >= (int) kFactory.size())
            return;

        const auto& f = kFactory[static_cast<size_t> (index)];

        auto ns = snapshotState().createCopy();

        // every param -> its default, then the genre overrides, then force synth-only.
        for (int i = 0; i < ns.getNumChildren(); ++i)
        {
            auto child = ns.getChild (i);
            const auto pId = child.getProperty ("id").toString();
            if (pId.isNotEmpty())
                child.setProperty ("value", defaultOf (pId), nullptr);
        }
        for (const auto& o : f.overrides)
            writeTreeParam (ns, o.id, o.value);

        writeTreeParam (ns, pid::synthEnable,  1.0f);
        writeTreeParam (ns, pid::sampleEnable, 0.0f);

        ns.setProperty ("currentPresetName", f.name, nullptr);
        ns.setProperty ("currentPresetPath", juce::String(), nullptr);
        // Factory presets are synth-only in SOUND (sampleEnable written 0 above), but they
        // keep whatever kick is currently loaded in the SAMPLE strip rather than clearing it
        // (bug-scan 2026-09-01, code-review CONFIRMED: writing "" here undid the fresh-
        // instance "first factory kick preloaded, section off" seeding the moment any factory
        // preset was loaded — the strip fell back to "N kicks in the bank" with nothing armed).
        ns.setProperty ("currentSampleName", getSampleName ? getSampleName() : juce::String(), nullptr);

        applyStateUndoable (std::move (ns), juce::String ("Preset: ") + f.name);
    }

    //======================================================================== user
    namespace { juce::File& testFolderRef() { static juce::File f; return f; } }

    void PresetManager::setUserFolderForTests (const juce::File& f)
    {
        testFolderRef() = f;
        if (f != juce::File())
            f.createDirectory();
    }

    juce::File PresetManager::userFolder()
    {
        if (testFolderRef() != juce::File())
            return testFolderRef();

        auto base = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        if (base == juce::File() || base.getFullPathName().isEmpty())
            base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

        auto folder = base.getChildFile ("KICKR").getChildFile ("Presets");
        if (! folder.createDirectory().wasOk() && ! folder.isDirectory())
            folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("KICKR").getChildFile ("Presets");
        folder.createDirectory();
        return folder;
    }

    juce::StringArray PresetManager::getUserNames() const
    {
        juce::StringArray names;
        for (const auto& f : juce::RangedDirectoryIterator (userFolder(), false, "*.kickrpreset",
                                                            juce::File::findFiles))
            names.add (f.getFile().getFileNameWithoutExtension());
        names.sortNatural();
        return names;
    }

    bool PresetManager::saveUser (const juce::String& name)
    {
        const auto safe = juce::File::createLegalFileName (name).trim();
        if (safe.isEmpty())
            return false;

        auto state = snapshotState();
        state.setProperty ("stateVersion", 2, nullptr);
        state.setProperty ("currentPresetName", safe, nullptr);
        state.setProperty ("currentSampleName", getSampleName ? getSampleName() : juce::String(), nullptr);

        const auto dest = userFolder().getChildFile (safe + ".kickrpreset");
        if (auto xml = std::unique_ptr<juce::XmlElement> (state.createXml()))
        {
            const bool ok = xml->writeTo (dest);
            if (ok)
                setPresetName (safe, dest);
            return ok;
        }
        return false;
    }

    bool PresetManager::loadUser (const juce::String& name)
    {
        const auto file = userFolder().getChildFile (name + ".kickrpreset");
        if (! file.existsAsFile())
            return false;

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr)
            return false;

        auto tree = juce::ValueTree::fromXml (*xml);
        if (! tree.isValid() || tree.getType() != apvts.state.getType())
            return false;

        tree.setProperty ("currentPresetName", name, nullptr);
        tree.setProperty ("currentPresetPath", file.getFullPathName(), nullptr);

        applyStateUndoable (std::move (tree), juce::String ("Preset: ") + name);
        return true;
    }

    bool PresetManager::loadByName (const juce::String& name)
    {
        const auto factory = getFactoryNames();
        const int  fi      = factory.indexOf (name);
        if (fi >= 0) { loadFactory (fi); return true; }
        return loadUser (name);
    }

    //================================================================ randomize/mutate
    void PresetManager::randomize()
    {
        auto ns = snapshotState().createCopy();

        for (const auto& r : kRandRanges)
        {
            if (juce::String (r.id) == pid::noiseLevel)
            {
                const float v = rng.nextFloat() < 0.60f ? 0.0f
                                                        : r.lo + rng.nextFloat() * (r.hi - r.lo);
                writeTreeParam (ns, r.id, v);
                continue;
            }
            writeTreeParam (ns, r.id, r.lo + rng.nextFloat() * (r.hi - r.lo));
        }
        writeTreeParam (ns, pid::noiseType, (float) rng.nextInt (3));

        // Light correlations.
        if (readTreeParam (ns, pid::drive) > 0.5f)
            writeTreeParam (ns, pid::character,
                            juce::jlimit (0.0f, 1.0f, readTreeParam (ns, pid::character) + rng.nextFloat() * 0.2f));
        if (readTreeParam (ns, pid::tailLength) > 400.0f)
            writeTreeParam (ns, pid::tailTone,
                            juce::jlimit (0.0f, 1.0f, readTreeParam (ns, pid::tailTone) - 0.15f));

        ns.setProperty ("currentPresetName", "Random", nullptr);
        applyStateUndoable (std::move (ns), "Randomize");
    }

    void PresetManager::mutate()
    {
        auto ns = snapshotState().createCopy();

        for (auto* p : apvts.processor.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
            if (rp == nullptr || dynamic_cast<juce::AudioParameterChoice*> (p) != nullptr)
                continue;

            const auto pId = rp->getParameterID();
            if (isExcludedFromRandom (pId))
                continue;

            const auto& range = rp->getNormalisableRange();
            const float span  = range.end - range.start;
            const float cur   = readTreeParam (ns, pId);
            const float delta = (rng.nextFloat() - 0.5f) * 0.24f * span;   // ~+-12 %
            writeTreeParam (ns, pId, juce::jlimit (range.start, range.end, cur + delta));
        }

        const auto n = getCurrentPresetName();
        ns.setProperty ("currentPresetName",
                        n.endsWith (" *") ? n : (n.isEmpty() ? juce::String ("Mutated") : n + " *"),
                        nullptr);
        applyStateUndoable (std::move (ns), "Mutate");
    }
}
