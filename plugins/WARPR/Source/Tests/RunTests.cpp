/*
    WARPR - console test runner.  Grows one block of checks per phase.
    Build:  cmake --build build --target WARPR_Tests
    Run:    ./build/plugins/WARPR/WARPR_Tests          (exit code 0 = all passed)

    Stage 1 (F1 + F2) blocks:
      1. parameter table vs APVTS   (135 ids, order, type, range, default, step, skew, automatable)
      2. editor relay macros vs table (55 + 80 = 135)
      3. state round trip           (random values + non-parameter model)
      4. state robustness           (truncated / mutated / garbage input)
      5. LockFreeSwap + RetireList  (threaded stress, back-pressure, no free on the audio thread)
      6. offline render + alloc hook + bus layouts (D1: audible default wave)
      7. scanners + FFT alias meter self-test
      8. FLAC writer API round trip (24-bit)

    Phase D1 block lives in PhaseD1Tests.cpp (runPhaseD1Tests), phase D2 in PhaseD2Tests.cpp
    (runPhaseD2Tests), phase D3 in PhaseD3Tests.cpp (runPhaseD3Tests; shared class-level machinery in
    WarpTestKit.h), phase D4 in PhaseD4Tests.cpp (runPhaseD4Tests: shaper curves, LUT/ADAA, backend),
    phase D5 in PhaseD5Tests.cpp (runPhaseD5Tests: standard filters 0-19, response display, switching),
    phase D7 in PhaseD7Tests.cpp (runPhaseD7Tests: granular engine, D7a CLASSIC + LOOP),
    phase D10 in PhaseD10Tests.cpp (runPhaseD10Tests: interpolation levels, CPU guard, budget determinism, memory audit,
    bench self-check; the `--bench` CPU table lives there too).
    Flags:  --full  widens the sweeps.
            --bench [--bench-full] [--bench-runs=N] [--bench-blocks=N] [--bench-filter=text]
                    prints the CPU table (one `BENCH ...` line per patch) instead of running the tests; use a Release build.
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"            // WARPR_*_PARAMS relay macro tables
#include "Parameters/ParamTable.h"
#include "DSP/WarpModeTable.h"
#include "DSP/FilterTypeTable.h"
#include "State/StateIO.h"
#include "Util/LockFreeSwap.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Bit-exact float equality is intentional in this harness (state round trips, FLAC decode,
// discrete-vs-continuous classification), so the warning is suppressed for this file only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

namespace
{
    int failures = 0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    void checkCount (int mismatches, const char* name)
    {
        if (mismatches == 0)
            check (true, name);
        else
        {
            std::printf ("  FAIL  %s (%d mismatches)\n", name, mismatches);
            ++failures;
        }
    }

    const warpr::ParamMeta* findMeta (const std::string& id)
    {
        for (const auto& m : warpr::kParamMeta)
            if (id == m.id)
                return &m;
        return nullptr;
    }

    float rawValue (juce::AudioProcessorValueTreeState& apvts, const char* id)
    {
        auto* a = apvts.getRawParameterValue (id);
        return a != nullptr ? a->load() : -9999.0f;
    }

    //==========================================================================
    // 1. parameter table vs APVTS
    //==========================================================================
    void testParameterTable()
    {
        std::printf ("\n[F2] parameter table vs APVTS\n");

        PluginProcessor proc;
        auto& params = proc.getParameters();

        check (params.size() == (int) warpr::kNumParams, "APVTS registers exactly 135 parameters");

        int order = 0, version = 0, kind = 0, range = 0, def = 0, step = 0, skew = 0, automatable = 0, choices = 0;
        int numAutomatable = 0, numNonAutomatable = 0;
        int nFloat = 0, nInt = 0, nBool = 0, nChoice = 0;

        for (size_t i = 0; i < warpr::kNumParams && (int) i < params.size(); ++i)
        {
            const auto& m = warpr::kParamMeta[i];
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (params[(int) i]);

            if (ranged == nullptr || ranged->paramID != m.id)
            {
                ++order;
                continue;
            }

            if (ranged->getVersionHint() != 1) ++version;

            bool kindOk = false;
            switch (m.kind)
            {
                case warpr::ParamKind::Float:  kindOk = dynamic_cast<juce::AudioParameterFloat*>  (ranged) != nullptr; ++nFloat;  break;
                case warpr::ParamKind::Int:    kindOk = dynamic_cast<juce::AudioParameterInt*>    (ranged) != nullptr; ++nInt;    break;
                case warpr::ParamKind::Bool:   kindOk = dynamic_cast<juce::AudioParameterBool*>   (ranged) != nullptr; ++nBool;   break;
                case warpr::ParamKind::Choice: kindOk = dynamic_cast<juce::AudioParameterChoice*> (ranged) != nullptr; ++nChoice; break;
            }
            if (! kindOk) ++kind;

            const auto& nr = ranged->getNormalisableRange();
            if (std::abs (nr.start - m.minValue) > 1.0e-6f || std::abs (nr.end - m.maxValue) > 1.0e-6f) ++range;

            const float defaultValue = ranged->convertFrom0to1 (ranged->getDefaultValue());
            if (std::abs (defaultValue - m.defaultValue) > 1.0e-4f * std::max (1.0f, std::abs (m.defaultValue))) ++def;

            if (m.kind == warpr::ParamKind::Float)
            {
                if (std::abs (nr.interval - m.step) > 1.0e-7f) ++step;

                if (m.skewCentre > 0.0f)
                {
                    const float atHalf = ranged->convertFrom0to1 (0.5f);
                    if (std::abs (atHalf - m.skewCentre) > 1.0e-3f * m.skewCentre) ++skew;
                }
                else if (std::abs (nr.skew - 1.0f) > 1.0e-6f)
                    ++skew;
            }

            if (ranged->isAutomatable() != m.automatable) ++automatable;
            if (ranged->isAutomatable()) ++numAutomatable; else ++numNonAutomatable;

            if (m.kind == warpr::ParamKind::Choice)
                if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (ranged))
                    if (c->choices.size() != m.numChoices) ++choices;
        }

        checkCount (order,       "ids match the table in exact layout order");
        checkCount (version,     "every ParameterID has version hint 1");
        checkCount (kind,        "JUCE parameter class matches the table kind");
        checkCount (range,       "ranges match");
        checkCount (def,         "defaults match");
        checkCount (step,        "float intervals match");
        checkCount (skew,        "skew centres match (setSkewForCentre) / linear elsewhere");
        checkCount (automatable, "automatable flag matches");
        checkCount (choices,     "choice list sizes match");
        check (nFloat == 62 && nInt == 5 && nBool == 5 && nChoice == 63, "types: Float 62, Int 5, Bool 5, Choice 63");
        check (numAutomatable == 115 && numNonAutomatable == 20, "115 automatable / 20 non-automatable (lfoNGridX/Y)");

        // non-automatable set is exactly lfoNGridX / lfoNGridY
        int wrongSet = 0;
        for (const auto& m : warpr::kParamMeta)
        {
            const std::string id (m.id);
            const bool isGrid = id.rfind ("lfo", 0) == 0
                             && (id.size() > 5 && (id.substr (id.size() - 5) == "GridX" || id.substr (id.size() - 5) == "GridY"));
            if (isGrid == m.automatable) ++wrongSet;
        }
        checkCount (wrongSet, "non-automatable set == the 20 lfoNGridX/lfoNGridY ids");

        // spot checks against parameter-spec.md literals
        auto choiceOf = [&proc] (const char* id) -> juce::AudioParameterChoice*
        {
            return dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id));
        };

        auto* warpMode   = choiceOf ("warpMode");
        auto* filterType = choiceOf ("filterType");
        auto* bendRange  = choiceOf ("bendRange");

        check (warpMode != nullptr && warpMode->choices.size() == 60
               && warpMode->choices[0] == "Bend Positive" && warpMode->choices[17] == "Sine Fold"
               && warpMode->choices[43] == "Quantization" && warpMode->choices[59] == "Cross Mod"
               && warpMode->getIndex() == 17,
               "warpMode: 60 entries, default Sine Fold (17)");
        check (filterType != nullptr && filterType->choices.size() == 34
               && filterType->choices[3] == "LP 24" && filterType->choices[19] == "High Shelf"
               && filterType->choices[21] == juce::String::fromUTF8 ("Comb \xE2\x88\x92")
               && filterType->choices[33] == "Ring Mod" && filterType->getIndex() == 3,
               "filterType: 34 entries, default LP 24 (3), index 21 = 'Comb' + U+2212");
        check (bendRange != nullptr && bendRange->choices.size() == 3
               && bendRange->choices[1] == juce::String::fromUTF8 ("\xC2\xB1" "12"),
               "bendRange: three UTF-8 entries");

        auto* lfo10GridX = choiceOf ("lfo10GridX");
        auto* lfo1Sync   = choiceOf ("lfo1Sync");
        check (lfo10GridX != nullptr && lfo10GridX->choices[lfo10GridX->getIndex()] == "8" && ! lfo10GridX->isAutomatable(),
               "lfo10GridX defaults to '8' and is not automatable");
        check (lfo1Sync != nullptr && lfo1Sync->choices[lfo1Sync->getIndex()] == "Sync", "lfo1Sync defaults to Sync");

        // removed-from-earlier-mockups parameters must NOT exist
        int removedPresent = 0;
        for (const char* id : { "loopXfade", "startRandom", "playbackSpeed", "sampleGain", "filterGain", "fltAttack",
                                "fltDecay", "fltSustain", "fltRelease", "filterEnvAmount", "grainPitchJitter", "grainWindow" })
            if (proc.apvts.getParameter (id) != nullptr) ++removedPresent;
        checkCount (removedPresent, "parameters removed from earlier mockups are absent");

        // ParamSnapshot: negative ints round-trip (transpose = -24), block copy matches
        auto* transpose = proc.apvts.getParameter ("transpose");
        if (transpose != nullptr)
            transpose->setValueNotifyingHost (0.0f);
        warpr::ParamBlock block;
        proc.getParamSnapshot().read (block);
        check (block.getInt (warpr::ParamId::transpose) == -24, "ParamSnapshot: transpose = -24 reads back as -24");
        check (block.getInt (warpr::ParamId::rootNote) == 60 && block.getBool (warpr::ParamId::warpEnable)
               && ! block.getBool (warpr::ParamId::shaperEnable), "ParamSnapshot: defaults (rootNote 60, warpEnable on, shaperEnable off)");
    }

    //==========================================================================
    // 2. editor relay macro tables vs the table
    //==========================================================================
    void testEditorTables()
    {
        std::printf ("\n[F2] editor relay tables (v9 WARPR_*_PARAMS) vs ParamTable\n");

        std::set<std::string> ids;
        int duplicates = 0, wrongKind = 0, unknown = 0;
        int nSlider = 0, nToggle = 0, nCombo = 0;

        auto add = [&] (const char* id, bool (*kindOk) (warpr::ParamKind))
        {
            if (! ids.insert (id).second) ++duplicates;
            const auto* m = findMeta (id);
            if (m == nullptr) ++unknown;
            else if (! kindOk (m->kind)) ++wrongKind;
        };

        auto isSlider = [] (warpr::ParamKind k) { return k == warpr::ParamKind::Float || k == warpr::ParamKind::Int; };
        auto isToggle = [] (warpr::ParamKind k) { return k == warpr::ParamKind::Bool; };
        auto isCombo  = [] (warpr::ParamKind k) { return k == warpr::ParamKind::Choice; };

       #define X(id) add (#id, isSlider); ++nSlider;
        WARPR_SLIDER_PARAMS (X)
       #undef X
       #define X(id) add (#id, isToggle); ++nToggle;
        WARPR_TOGGLE_PARAMS (X)
       #undef X
       #define X(id) add (#id, isCombo); ++nCombo;
        WARPR_COMBO_PARAMS (X)
       #undef X

        check (nSlider == 37 && nToggle == 5 && nCombo == 13, "macro tables: 37 slider + 5 toggle + 13 combo = 55");

        // the editor builds 10 x 8 LFO relays from these suffixes (see PluginEditor.cpp)
        for (int n = 1; n <= 10; ++n)
        {
            const std::string p = "lfo" + std::to_string (n);
            for (const char* s : { "Mode", "Dir", "Sync", "GridX", "GridY" }) add ((p + s).c_str(), isCombo);
            for (const char* s : { "Rate", "Phase", "Smooth" })               add ((p + s).c_str(), isSlider);
        }

        checkCount (duplicates, "no duplicate relay ids");
        checkCount (unknown,    "every relay id exists in the table");
        checkCount (wrongKind,  "relay class matches parameter kind (slider: Float/Int, toggle: Bool, combo: Choice)");
        check (ids.size() == warpr::kNumParams, "relay ids cover exactly 135 parameters");

        int missing = 0;
        for (const auto& m : warpr::kParamMeta)
            if (ids.count (m.id) == 0) ++missing;
        checkCount (missing, "every table parameter has an editor relay");
    }

    //==========================================================================
    // 3. state round trip
    //==========================================================================
    void testStateRoundTrip()
    {
        std::printf ("\n[F2] state round trip\n");

        PluginProcessor a;
        juce::Random rng (20261111);

        for (auto* p : a.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
                r->setValueNotifyingHost (rng.nextFloat());

        int moved = 0;
        for (const auto& m : warpr::kParamMeta)
            if (std::abs (rawValue (a.apvts, m.id) - m.defaultValue) > 1.0e-6f) ++moved;
        check (moved > 100, "randomisation moved most parameters off their defaults");

        auto& backendA = a.getBackendImpl();
        backendA.setLfoCount (7);
        backendA.setLfoShapeJson (2, R"({"preset":"SAW UP","points":[],"bends":[{"x":0.5,"v":0.25}],"table":null,"drift":null})");
        backendA.setModMatrixJson (R"([{"src":2,"dst":"filterCutoff","depth":0.5}])");
        backendA.setShaperCurveJson ("B", R"({"interp":"linear","points":[{"x":-1,"y":-1},{"x":0.5,"y":0.8},{"x":1,"y":1}]})");

        juce::MemoryBlock blob, blobAgain;
        a.getStateInformation (blob);
        a.getStateInformation (blobAgain);
        check (blob.getSize() > 16 && blob == blobAgain, "getStateInformation is deterministic");

        PluginProcessor b;
        const auto revisionBefore = b.getUiBackend().revision (WarprUiBackend::Revision::uiState);
        b.setStateInformation (blob.getData(), (int) blob.getSize());
        check (b.getUiBackend().revision (WarprUiBackend::Revision::uiState) > revisionBefore, "state load bumps Revision::uiState");

        int discrete = 0, continuous = 0;
        float worst = 0.0f;
        for (const auto& m : warpr::kParamMeta)
        {
            const float x = rawValue (a.apvts, m.id), y = rawValue (b.apvts, m.id);
            if (m.kind == warpr::ParamKind::Float)
            {
                const float dev = std::abs (x - y) / std::max (1.0f, std::abs (x));
                worst = std::max (worst, dev);
                if (dev > 1.0e-5f) ++continuous;
            }
            else if (x != y)
                ++discrete;
        }
        checkCount (discrete,   "Int/Bool/Choice parameters round-trip exactly");
        checkCount (continuous, "Float parameters round-trip within float rounding (1e-5 relative)");
        std::printf ("        (worst float deviation %.3g)\n", (double) worst);

        const auto ma = backendA.copyModel(), mb = b.getBackendImpl().copyModel();
        check (ma.lfoCount == mb.lfoCount && ma.lfoShapeJson == mb.lfoShapeJson && ma.modMatrixJson == mb.modMatrixJson
               && ma.shaperCurveJson == mb.shaperCurveJson && ma.presetName == mb.presetName,
               "non-parameter model (WARPR_UI_STATE) round-trips verbatim");

        const auto ui = juce::JSON::parse (b.getUiBackend().getUiStateJson());
        const auto* shapes = ui.getProperty ("lfoShapes", {}).getArray();
        check (ui.getProperty ("lfoCount", 0) == juce::var (7) && shapes != nullptr && shapes->size() == 7,
               "getUiStateJson: lfoCount 7 and 7 lfoShapes");
        check (ui.getProperty ("modMatrix", {}).isArray() && ui.getProperty ("shaperCurves", {}).getDynamicObject() != nullptr,
               "getUiStateJson: modMatrix array + shaperCurves object");

        // shaper echo-avoidance contract
        auto& be = b.getUiBackend();
        const auto before = be.revision (WarprUiBackend::Revision::shaperCurves);
        be.setShaperCurveJson ("A", "{}");
        check (be.revision (WarprUiBackend::Revision::shaperCurves) == before, "setShaperCurveJson does NOT bump Revision::shaperCurves");
        be.shaperReset ("A");
        check (be.revision (WarprUiBackend::Revision::shaperCurves) == before + 1, "shaperReset bumps Revision::shaperCurves");
        check (juce::JSON::parse (be.getShaperCurveJson ("A")).getDynamicObject() != nullptr, "getShaperCurveJson returns the identity document after reset");
    }

    //==========================================================================
    // 4. state robustness
    //==========================================================================
    bool allParamsSane (PluginProcessor& p)
    {
        for (const auto& m : warpr::kParamMeta)
        {
            const float v = rawValue (p.apvts, m.id);
            if (! std::isfinite (v) || v < m.minValue - 1.0e-3f || v > m.maxValue + 1.0e-3f)
                return false;
        }
        return true;
    }

    void testStateRobustness()
    {
        std::printf ("\n[F2] state robustness (truncated / mutated / garbage)\n");

        PluginProcessor source;
        juce::MemoryBlock blob;
        source.getStateInformation (blob);

        PluginProcessor victim;
        bool sane = true;

        // every short prefix, then a coarse sweep up to the full length
        for (size_t len = 0; len < blob.getSize(); len += (len < 96 ? (size_t) 1 : (size_t) 61))
        {
            victim.setStateInformation (blob.getData(), (int) len);
            sane = sane && allParamsSane (victim);
        }
        check (sane, "truncated states (all prefixes) never crash and leave sane parameters");

        juce::Random rng (777);
        sane = true;
        for (int iter = 0; iter < 400; ++iter)
        {
            juce::MemoryBlock mutated (blob);
            auto* bytes = static_cast<juce::uint8*> (mutated.getData());
            const int flips = 1 + rng.nextInt (8);
            for (int f = 0; f < flips; ++f)
                bytes[rng.nextInt ((int) mutated.getSize())] = (juce::uint8) rng.nextInt (256);

            victim.setStateInformation (mutated.getData(), (int) mutated.getSize());
            sane = sane && allParamsSane (victim);
        }
        check (sane, "400 randomly mutated states never crash and leave sane parameters");

        sane = true;
        for (int iter = 0; iter < 100; ++iter)
        {
            juce::MemoryBlock garbage ((size_t) (1 + rng.nextInt (512)));
            auto* bytes = static_cast<juce::uint8*> (garbage.getData());
            for (size_t i = 0; i < garbage.getSize(); ++i)
                bytes[i] = (juce::uint8) rng.nextInt (256);

            victim.setStateInformation (garbage.getData(), (int) garbage.getSize());
            sane = sane && allParamsSane (victim);
        }
        check (sane, "100 garbage blobs never crash and leave sane parameters");

        victim.setStateInformation (nullptr, 0);
        victim.setStateInformation (blob.getData(), -5);
        check (allParamsSane (victim), "null / negative-size input is ignored");

        // legacy plain-XML fallback path
        PluginProcessor legacy;
        auto xml = source.apvts.copyState().createXml();
        warpr::UiModel model;
        check (xml != nullptr && warpr::state::readXml (*xml, legacy.apvts, model), "plain-XML (legacy) state is accepted");
        juce::XmlElement wrongRoot ("NotWarpr");
        check (! warpr::state::readXml (wrongRoot, legacy.apvts, model), "foreign XML root is rejected");
    }

    //==========================================================================
    // 5. LockFreeSwap + RetireList
    //==========================================================================
    thread_local bool tIsAudioThread = false;

    struct Probe
    {
        explicit Probe (int s) : serial (s) { ++created; }
        ~Probe() { ++destroyed; if (tIsAudioThread) ++destroyedOnAudio; }

        int serial;
        static inline std::atomic<int> created { 0 }, destroyed { 0 }, destroyedOnAudio { 0 };
        static void reset() { created = 0; destroyed = 0; destroyedOnAudio = 0; }
    };

    void testSwapPrimitives()
    {
        std::printf ("\n[F2] LockFreeSwap + RetireList\n");

        // --- threaded stress: message thread publishes, audio thread consumes ----------------
        Probe::reset();
        int adopted = 0, lastSerial = -1, finalSerial = -2;
        bool monotonic = true;
        constexpr int total = 100000;

        {
            warpr::RetireList retire;
            warpr::LockFreeSwap<Probe> swap (retire);
            std::atomic<bool> done { false };

            std::thread audio ([&]
            {
                tIsAudioThread = true;
                long long sink = 0;
                while (! done.load())
                {
                    if (swap.acquire())
                    {
                        ++adopted;
                        const int s = swap.get()->serial;
                        if (s <= lastSerial) monotonic = false;
                        lastSerial = s;
                    }
                    if (auto* p = swap.get())
                        sink += p->serial;
                }
                tIsAudioThread = false;
                if (sink == -1) std::printf ("unreachable\n");   // keep the read observable
            });

            for (int i = 0; i < total; ++i)
            {
                swap.publish (std::make_unique<Probe> (i));
                if ((i & 15) == 0)
                    retire.drain();
            }

            done = true;
            audio.join();

            // the audio thread has exited: adopt whatever is still pending, from this thread
            for (int guard = 0; guard < 4; ++guard)
            {
                retire.drain();
                swap.acquire();
            }
            if (swap.get() != nullptr) finalSerial = swap.get()->serial;
        }   // swap destroyed (pending + current released), then the retire list drains the rest

        check (Probe::created == total && Probe::destroyed == total, "every published object is destroyed exactly once");
        check (Probe::destroyedOnAudio == 0, "no object was ever destroyed on the audio thread");
        check (monotonic && adopted > 0, "audio thread adopts publications in order");
        check (finalSerial == total - 1, "the most recent publication ends up current");
        std::printf ("        (audio thread adopted %d of %d publications)\n", adopted, total);

        // --- back-pressure: a full retire list keeps the old object, never frees ---------------
        Probe::reset();
        {
            warpr::RetireList retire;
            warpr::LockFreeSwap<Probe> swap (retire);
            int refused = 0;
            for (int i = 0; i < 200; ++i)
            {
                tIsAudioThread = false;                      // publishing is a message-thread action
                swap.publish (std::make_unique<Probe> (i));

                tIsAudioThread = true;                       // acquire() is the audio-thread action
                if (! swap.acquire()) ++refused;
                tIsAudioThread = false;
            }

            check (retire.numPending() <= warpr::RetireList::capacity - 1, "retire list never overfills");
            check (refused > 0, "acquire() refuses (back-pressure) when the retire list is full");
            check (Probe::destroyedOnAudio == 0, "back-pressure path never frees on the audio thread");

            retire.drain();
            check (swap.acquire() && swap.get()->serial == 199, "after draining, the pending publication is adopted");
        }
        check (Probe::created == Probe::destroyed, "no leaks after back-pressure test");
    }

    //==========================================================================
    // 6. offline render, allocation hook, bus layouts
    //==========================================================================
    void testProcessorShell()
    {
        std::printf ("\n[F1] processor shell: buses, silence, allocations\n");

        PluginProcessor proc;
        check (proc.acceptsMidi() && ! proc.producesMidi() && ! proc.isMidiEffect(), "instrument: accepts MIDI, produces none");
        check (proc.getTotalNumInputChannels() == 0 && proc.getTotalNumOutputChannels() == 2, "no input bus, one stereo output bus");

        juce::AudioProcessor::BusesLayout stereoOut;
        stereoOut.outputBuses.add (juce::AudioChannelSet::stereo());
        juce::AudioProcessor::BusesLayout monoOut;
        monoOut.outputBuses.add (juce::AudioChannelSet::mono());
        juce::AudioProcessor::BusesLayout withInput = stereoOut;
        withInput.inputBuses.add (juce::AudioChannelSet::stereo());
        check (proc.checkBusesLayoutSupported (stereoOut) && ! proc.checkBusesLayoutSupported (monoOut)
               && ! proc.checkBusesLayoutSupported (withInput),
               "isBusesLayoutSupported: stereo out only (mono and input buses rejected)");

        constexpr double fs = 48000.0;
        const auto midi = warpr::tests::singleNote (60, 0.8f, fs, 0.5);
        const auto out = warpr::tests::render (proc, midi, fs, 256, 1.0);

        check (out.getNumChannels() == 2 && out.getNumSamples() == 48000, "offline render: 2 channels x 1 s");

        bool finite = true, audible = true;
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
        {
            const auto s = warpr::tests::scan (out, ch);
            finite = finite && s.allFinite;
            audible = audible && s.peak > 0.05f && s.peak <= 1.0f;
        }
        check (finite, "render is finite (setPlayConfigDetails before prepareToPlay)");
        check (audible, "D1 processBlock plays the default wave (audible, within the safety knee)");

        // zero allocations in processBlock
        proc.setPlayConfigDetails (0, 2, fs, 256);
        proc.prepareToPlay (fs, 256);
        juce::AudioBuffer<float> buffer (2, 256);
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 64, 0.9f), 0);
        proc.processBlock (buffer, m);                       // warm-up

        std::size_t allocations = 1;
        {
            warpr::tests::ScopedAllocCount counter;
            for (int i = 0; i < 100; ++i)
                proc.processBlock (buffer, m);
            allocations = counter.count();
        }
        check (allocations == 0, "processBlock performs zero allocations");
        proc.releaseResources();
    }

    //==========================================================================
    // 7. scanners + alias meter
    //==========================================================================
    void testScanners()
    {
        std::printf ("\n[F1] scanners + FFT alias meter\n");

        std::vector<float> x { 0.5f, 0.25f, 0.5f, -0.5f };
        auto s = warpr::tests::scan (x.data(), (int) x.size());
        check (s.allFinite && std::abs (s.peak - 0.5f) < 1.0e-6f && std::abs (s.maxStep - 1.0f) < 1.0e-6f
               && std::abs (s.mean - 0.1875) < 1.0e-9, "scan(): peak, max step, DC");

        x[1] = std::numeric_limits<float>::quiet_NaN();
        check (! warpr::tests::scan (x.data(), (int) x.size()).allFinite, "scan(): detects NaN");

        constexpr double fs = 48000.0;
        constexpr int order = 15, size = 1 << order;
        const double f0 = fs * 400.0 / size;                 // exact bin centre
        const double f1 = fs * 1234.0 / size;                // inharmonic tone
        std::vector<float> clean ((size_t) size + 5000), dirty (clean.size());
        for (size_t i = 0; i < clean.size(); ++i)
        {
            const double t = (double) i / fs;
            clean[i] = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * f0 * t));
            dirty[i] = clean[i] + (float) (0.005 * std::sin (2.0 * juce::MathConstants<double>::pi * f1 * t));
        }

        const double cleanDb = warpr::tests::aliasDb (clean.data(), (int) clean.size(), fs, f0, order);
        const double dirtyDb = warpr::tests::aliasDb (dirty.data(), (int) dirty.size(), fs, f0, order);
        std::printf ("        (clean %.1f dB, dirty %.1f dB)\n", cleanDb, dirtyDb);
        check (cleanDb < -90.0, "alias meter: clean bin-centred sine measures below -90 dB");
        check (dirtyDb > -45.0 && dirtyDb < -35.0, "alias meter: -40 dB inharmonic tone measures about -40 dB");
    }

    //==========================================================================
    // 8. FLAC writer API (JUCE 8.0.15 AudioFormatWriterOptions) round trip
    //==========================================================================
    void testFlacRoundTrip()
    {
        std::printf ("\n[F1] FLAC 24-bit writer/reader round trip (JUCE AudioFormatWriterOptions)\n");

        constexpr int n = 2000;
        juce::AudioBuffer<float> in (2, n);
        for (int i = 0; i < n; ++i)
        {
            // exact multiples of 2^-23 -> representable in 24-bit PCM and in float
            in.setSample (0, i, (float) (((i * 37) % 8001 - 4000) * 1000) / 8388608.0f);
            in.setSample (1, i, (float) (((i * 91) % 6001 - 3000) * 1000) / 8388608.0f);
        }

        juce::FlacAudioFormat flac;
        juce::MemoryBlock encoded;
        bool wrote = false;
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (encoded, false);
            auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                            .withSampleRate (48000.0)
                                                            .withNumChannels (2)
                                                            .withBitsPerSample (24));
            wrote = writer != nullptr && writer->writeFromAudioSampleBuffer (in, 0, n);
        }                                                   // writer destroyed -> FLAC stream finalised
        check (wrote && encoded.getSize() > 0, "FLAC 24-bit writer produced data");

        std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (new juce::MemoryInputStream (encoded, false), true));
        bool exact = false;
        if (reader != nullptr && reader->lengthInSamples == n && reader->numChannels == 2)
        {
            juce::AudioBuffer<float> out (2, n);
            reader->read (&out, 0, n, 0, true, true);
            exact = true;
            for (int ch = 0; ch < 2 && exact; ++ch)
                for (int i = 0; i < n; ++i)
                    if (out.getSample (ch, i) != in.getSample (ch, i)) { exact = false; break; }
        }
        check (exact, "FLAC 24-bit decode is bit-exact for 24-bit-representable samples");
    }

    //==========================================================================
    // G2: choice lists the WebView builds its selects from + the filter display feed for every type
    //==========================================================================
    void testG2ChoiceListsAndFilterFeed()
    {
        std::printf ("\n[G2] choice lists (APVTS -> UI selects) and filter response feed\n");

        PluginProcessor proc;
        auto choiceOf = [&proc] (const char* id) { return dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)); };
        auto* warpMode   = choiceOf ("warpMode");
        auto* filterType = choiceOf ("filterType");

        // index.html keeps ONLY the group boundaries: warp 15/10/19/10/6, filter 20/14 - the sums must equal the APVTS lists
        constexpr int warpGroups[] = { 15, 10, 19, 10, 6 }, filterGroups[] = { 20, 14 };
        int wsum = 0, fsum = 0;
        for (int g : warpGroups)   wsum += g;
        for (int g : filterGroups) fsum += g;
        check (warpMode != nullptr && filterType != nullptr
               && warpMode->choices.size() == wsum && filterType->choices.size() == fsum
               && wsum == warpr::kNumWarpModes && fsum == warpr::kNumFilterTypes,
               "warpMode 60 / filterType 34 choices == the UI group tables (15+10+19+10+6, 20+14)");

        int nameMismatch = 0;
        if (warpMode != nullptr)
            for (int i = 0; i < warpr::kNumWarpModes; ++i)
                if (warpMode->choices[i] != juce::String::fromUTF8 (warpr::kWarpModes[(size_t) i].name)) ++nameMismatch;
        if (filterType != nullptr)
            for (int i = 0; i < warpr::kNumFilterTypes; ++i)
                if (filterType->choices[i] != juce::String::fromUTF8 (warpr::kFilterTypes[(size_t) i].name)) ++nameMismatch;
        checkCount (nameMismatch, "every APVTS warp / filter entry names its DSP table row");

        check (warpMode != nullptr && warpMode->choices[15] == "Linear Fold" && warpMode->choices[25] == "Hard Clip"
               && warpMode->choices[44] == "Phase Distortion" && warpMode->choices[54] == "FM"
               && filterType != nullptr && filterType->choices[20] == "Comb +",
               "group boundaries land on the first entry of each group");

        // filter display feed: every type, a few cutoff / res / morph combos -> finite, inside the display range
        auto& be = proc.getBackendImpl();
        be.setSampleRate (48000.0);
        auto setParam = [&proc] (const char* id, float real)
        {
            if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)))
                q->setValueNotifyingHost (q->convertTo0to1 (real));
        };
        int bad = 0, flatTypes = 0;
        std::array<float, 256> db {};
        for (int t = 0; t < warpr::kNumFilterTypes; ++t)
        {
            bool anyNonFlat = false;
            for (float cutoff : { 200.0f, 1000.0f, 8000.0f })
                for (float res : { 0.0f, 0.6f, 1.0f })
                    for (float morph : { 0.0f, 0.5f, 1.0f })
                    {
                        setParam ("filterType", (float) t); setParam ("filterCutoff", cutoff);
                        setParam ("filterResonance", res); setParam ("filterMorph", morph);
                        be.computeFilterResponse (db.data(), 256);
                        for (float v : db)
                        {
                            if (! std::isfinite (v) || v < -48.0f || v > 24.0f) { ++bad; break; }
                            if (v != 0.0f) anyNonFlat = true;
                        }
                    }
            if (! anyNonFlat) { ++flatTypes; std::printf ("    type %d (%s) draws flat in every probe\n", t, warpr::kFilterTypes[(size_t) t].name); }
        }
        checkCount (bad, "computeFilterResponse: all 34 types x 27 settings finite and inside [-48, +24] dB");
        check (flatTypes <= 3, "at most the intrinsically flat types (all-pass family) draw as a flat line");
    }
}

//==============================================================================
int runPhaseD1Tests (bool full);              // Tests/PhaseD1Tests.cpp
int runPhaseD2Tests (bool full);              // Tests/PhaseD2Tests.cpp
int runPhaseD3Tests (bool full);              // Tests/PhaseD3Tests.cpp
int runPhaseD4Tests (bool full);              // Tests/PhaseD4Tests.cpp
int runPhaseD5Tests (bool full);              // Tests/PhaseD5Tests.cpp
int runPhaseD6Tests (bool full);              // Tests/PhaseD6Tests.cpp
int runPhaseD7Tests (bool full);              // Tests/PhaseD7Tests.cpp
int runPhaseD8Tests (bool full);              // Tests/PhaseD8Tests.cpp
int runPhaseD9Tests (bool full);              // Tests/PhaseD9Tests.cpp
int runPreviewTests (bool full);              // Tests/PreviewTests.cpp
int runPhaseD10Tests (bool full);             // Tests/PhaseD10Tests.cpp
int runPhaseD11Tests (bool full);             // Tests/PhaseD11Tests.cpp  (slice mode)
int runBenchmark (int argc, char** argv);     // Tests/PhaseD10Tests.cpp  (--bench)

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    bool full = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "--full")
            full = true;
        else if (arg == "--bench")
            return runBenchmark (argc, argv);      // D10: CPU table only (no tests); options: --bench-full --bench-runs=N --bench-blocks=N --bench-filter=text
    }

    std::printf ("WARPR tests  (Stage 1: Foundation + Shell, Stage 2 / D1: sample engine + voices)%s\n",
                 full ? "  [--full]" : "  [quick; --full widens the sweeps]");

    testParameterTable();
    testEditorTables();
    testStateRoundTrip();
    testStateRobustness();
    testSwapPrimitives();
    testProcessorShell();
    testScanners();
    testFlacRoundTrip();
    testG2ChoiceListsAndFilterFeed();

    failures += runPhaseD1Tests (full);
    failures += runPhaseD2Tests (full);
    failures += runPhaseD3Tests (full);
    failures += runPhaseD4Tests (full);
    failures += runPhaseD5Tests (full);
    failures += runPhaseD6Tests (full);
    failures += runPhaseD7Tests (full);
    failures += runPhaseD8Tests (full);
    failures += runPhaseD9Tests (full);
    failures += runPreviewTests (full);
    failures += runPhaseD10Tests (full);
    failures += runPhaseD11Tests (full);

    std::printf ("\n%s  (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
