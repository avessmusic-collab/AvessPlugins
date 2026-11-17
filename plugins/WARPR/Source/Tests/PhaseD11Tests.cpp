/*
    WARPR - phase D11 test block: SLICE MODE (engine + backend half).

    Called from RunTests.cpp main():   failures += runPhaseD11Tests (full);

    [slice] sections
      1. SliceMap parse / validate / cap / malformed-off / note lookup
      2. backend: setSliceMapJson stores verbatim, getUiStateJson serves it, malformed = off
      3. region latch EXACT: slice k at note base+k == the same engine playing the full sample with the global start / end set to
         the slice's edges + One-Shot + the root note (bit-exact), for all 6 slices; loopMode / playMode parameters do not matter
      4. UNPITCHED across notes (measured Hz on a sine, transpose still applies, an unsliced note IS pitched)
      5. notes outside [base, base + N) are silent; "on":false = ordinary playback
      6. note-off / sustain pedal ignored + auto-release + voice freed (the one-shot gates)
      7. map replacement mid-note leaves sounding voices on their latched slices
      8. Mono / Legato: a slice note is never a retarget (no glide, no pitch change)
      9. uiState round trip (getUiStateJson -> captureState -> fresh processor -> identical map + same audio), preset file
         persistence, a state without a map clears slicing
     10. determinism (two fresh engines, poly + mono sequences)
     11. zero allocations on the audio thread (note-ons, map adoption, voice ends)
     12. computeSliceEdges / autoSliceEdges: equal division, transient detection on a synthetic click train (printed onsets), gap,
         fallback, region restriction, backend path

    House rules: setPlayConfigDetails BEFORE prepareToPlay, deterministic randomness, zero allocations proven by AllocHook.
*/

#include "PluginProcessor.h"
#include "DSP/SampleData.h"
#include "DSP/SliceMap.h"
#include "Tests/AllocHook.h"
#include "Tests/Scanners.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Bit-exact float equality is the point of several assertions in this file, so the warning is suppressed here only.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD11Tests (bool full);

namespace
{
    int failures = 0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr double kPi = 3.14159265358979323846;

    //==========================================================================
    // helpers
    //==========================================================================
    void setParam (PluginProcessor& p, const char* id, float value)
    {
        if (auto* prm = p.apvts.getParameter (id))
        {
            prm->setValueNotifyingHost (prm->convertTo0to1 (value));
        }
        else
        {
            std::printf ("  FAIL  unknown parameter '%s'\n", id);
            ++failures;
        }
    }

    float rawOf (PluginProcessor& p, const char* id)
    {
        auto* a = p.apvts.getRawParameterValue (id);
        return a != nullptr ? a->load() : -9999.0f;
    }

    /** The value the parameter actually holds after being asked for `v` (0.001 step snapping). */
    float legal (PluginProcessor& p, const char* id, float v)
    {
        setParam (p, id, v);
        return rawOf (p, id);
    }

    struct Audio
    {
        std::vector<float> l, r;

        void append (const juce::AudioBuffer<float>& b)
        {
            l.insert (l.end(), b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
            r.insert (r.end(), b.getReadPointer (1), b.getReadPointer (1) + b.getNumSamples());
        }

        float peakFrom (size_t first) const
        {
            float p = 0.0f;
            for (size_t i = first; i < l.size(); ++i)
                p = std::max (p, std::max (std::abs (l[i]), std::abs (r[i])));
            return p;
        }

        float peak() const { return peakFrom (0); }
    };

    bool identical (const Audio& a, const Audio& b)
    {
        if (a.l.size() != b.l.size() || a.r.size() != b.r.size())
            return false;

        for (size_t i = 0; i < a.l.size(); ++i)
            if (a.l[i] != b.l[i] || a.r[i] != b.r[i])
                return false;

        return true;
    }

    juce::MidiBuffer noteOnEv (int note, float vel = 1.0f)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, note, vel), 0);
        return m;
    }

    juce::MidiBuffer noteOffEv (int note)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        return m;
    }

    juce::MidiBuffer ccEv (int number, int value)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::controllerEvent (1, number, value), 0);
        return m;
    }

    /** A processor with the console-harness setup and a block stepper (state survives between calls). */
    struct Rig
    {
        explicit Rig (warpr::SampleData::Ptr sample = nullptr)
        {
            proc.getBackendImpl().setErrorReporter ([] (const juce::String&) {});     // no native alert in the console harness
            proc.setPlayConfigDetails (0, 2, kFs, kBlock);                            // BEFORE prepareToPlay
            proc.prepareToPlay (kFs, kBlock);

            if (sample != nullptr)
                proc.getSampleStore().publish (sample);

            block();                                                                  // adopts the sample
        }

        void block (const juce::MidiBuffer& midi = juce::MidiBuffer(), Audio* out = nullptr)
        {
            juce::MidiBuffer m (midi);
            buf.clear();
            proc.processBlock (buf, m);

            if ((++blockCounter & 7u) == 0)
                proc.drainRetired();                                                  // stands in for the 100 ms timer

            for (int ch = 0; ch < 2; ++ch)
                finite = finite && warpr::tests::scan (buf, ch).allFinite;

            if (out != nullptr)
                out->append (buf);
        }

        /** Publishes a slice map and runs one empty block so the audio thread adopts it. */
        void setSlices (const juce::String& json)
        {
            proc.getBackendImpl().setSliceMapJson (json);
            block();
        }

        /** One note: on at block 0, `blocks` blocks in total; an optional note-off at block `offAt`. */
        Audio play (int note, int blocks, int offAt = -1, float vel = 1.0f)
        {
            Audio a;

            for (int b = 0; b < blocks; ++b)
            {
                if (b == 0)
                    block (noteOnEv (note, vel), &a);
                else if (b == offAt)
                    block (noteOffEv (note), &a);
                else
                    block (juce::MidiBuffer(), &a);
            }

            return a;
        }

        PluginProcessor proc;
        juce::AudioBuffer<float> buf { 2, kBlock };
        bool finite = true;
        unsigned blockCounter = 0;
    };

    warpr::SampleData::Ptr makeNoise (int frames, int seed, const char* name)
    {
        juce::Random rng (seed);
        juce::AudioBuffer<float> b (1, frames);

        for (int i = 0; i < frames; ++i)
        {
            const float v = 0.6f * (rng.nextFloat() * 2.0f - 1.0f);
            b.setSample (0, i, std::round (v * 8388608.0f) / 8388608.0f);       // 24-bit grid: the state codec is exact
        }

        return warpr::SampleData::create (b, kFs, name, false, false);
    }

    warpr::SampleData::Ptr makeSine (int frames, double hz, float amp)
    {
        juce::AudioBuffer<float> b (1, frames);

        for (int i = 0; i < frames; ++i)
            b.setSample (0, i, amp * (float) std::sin (2.0 * kPi * hz / kFs * (double) i));

        return warpr::SampleData::create (b, kFs, "sine", false, false);
    }

    juce::String mapJson (bool on, int base, const std::vector<float>& edges)
    {
        juce::String s = juce::String ("{\"on\":") + (on ? "true" : "false") + ",\"baseNote\":" + juce::String (base) + ",\"edges\":[";

        for (size_t i = 0; i < edges.size(); ++i)
            s += (i > 0 ? "," : "") + juce::String ((double) edges[i], 12);

        return s + "]}";
    }

    double estimateHz (const float* x, int n)
    {
        std::vector<double> t;

        for (int i = 1; i < n; ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                t.push_back ((double) (i - 1) + (double) (-x[i - 1]) / (double) (x[i] - x[i - 1]));

        if (t.size() < 4)
            return 0.0;

        return (double) (t.size() - 1) / (t.back() - t.front()) * kFs;
    }

    /** The 6-slice edge set (24000-frame sample): 3000 x 4, then 6000 x 2 frames - each value is a legal parameter value. */
    std::vector<float> legalEdges6()
    {
        Rig scratch;
        std::vector<float> e;

        for (float v : { 0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.75f, 1.0f })
            e.push_back (legal (scratch.proc, "sampleStart", v));

        return e;
    }

    constexpr int kFrames = 24000;
    constexpr int kRenderBlocks = 44;                     // > the longest slice (6000 frames = 24 blocks) + release

    //==========================================================================
    // 1. SliceMap document
    //==========================================================================
    void testMapDocument()
    {
        std::printf ("\n[slice] 1. SliceMap parse / validate / cap / malformed-off\n");
        using warpr::SliceMap;

        SliceMap m;
        check (SliceMap::parse (R"({"on":true,"baseNote":36,"edges":[0,0.25,0.5,1]})", m) && m.on && m.baseNote == 36
                   && m.count == 3 && m.edges[0] == 0.0f && m.edges[1] == 0.25f && m.edges[2] == 0.5f && m.edges[3] == 1.0f,
               "valid document parses (3 slices)");

        check (m.sliceFor (35) == -1 && m.sliceFor (36) == 0 && m.sliceFor (38) == 2 && m.sliceFor (39) == -1,
               "sliceFor: [base, base + N) maps to 0..N-1, everything else -1");

        SliceMap d;
        check (SliceMap::parse (R"({"on":true,"edges":[0,1]})", d) && d.baseNote == 36 && d.count == 1, "missing baseNote = 36");
        check (SliceMap::parse (R"({"on":true,"baseNote":200,"edges":[0,1]})", d) && d.baseNote == 127, "baseNote clamps to 127");
        check (SliceMap::parse (R"({"on":true,"baseNote":-5,"edges":[0,1]})", d) && d.baseNote == 0, "baseNote clamps to 0");
        check (SliceMap::parse (R"({"on":true,"baseNote":"x","edges":[0,1]})", d) && d.baseNote == 36, "non-numeric baseNote = 36");
        check (SliceMap::parse (R"({"on":1,"edges":[0,1]})", d) && d.on, "numeric on (1) is on");
        check (SliceMap::parse (R"({"on":0,"edges":[0,1]})", d) && ! d.on, "numeric on (0) is off");
        check (SliceMap::parse (R"({"edges":[0,1]})", d) && ! d.on, "missing on = off (document still valid)");
        check (SliceMap::parse (R"({"on":true,"edges":[-0.5,0.5,2]})", d) && d.edges[0] == 0.0f && d.edges[1] == 0.5f && d.edges[2] == 1.0f,
               "edges clamp to [0,1]");
        check (SliceMap::parse (R"({"on":true,"edges":[0.25,0.25,0.5]})", d) && d.count == 2 && d.sliceFor (36) == 0,
               "a zero-width slice keeps its note");

        // cap at 64 slices (65 edges)
        juce::String big = R"({"on":true,"baseNote":0,"edges":[)";
        for (int i = 0; i < 100; ++i)
            big += (i > 0 ? "," : "") + juce::String ((double) i / 99.0, 15);
        big += "]}";
        check (SliceMap::parse (big, d) && d.count == 64 && d.edges[64] == (float) ((double) 64 / 99.0) && d.sliceFor (63) == 63 && d.sliceFor (64) == -1,
               "100 edges are capped at 64 slices (the first 65 edges)");

        const char* malformed[] = { "", "garbage", "[]", "42", "null", R"({"on":true})", R"({"on":true,"edges":[]})",
                                    R"({"on":true,"edges":[0.5]})", R"({"on":true,"edges":[0,0.8,0.3]})",
                                    R"({"on":true,"edges":[0,"a",1]})", R"({"on":true,"edges":"0,1"})",
                                    R"({"on":true,"edges":[0,null,1]})" };
        bool allOff = true;
        for (const char* text : malformed)
        {
            SliceMap probe;
            probe.on = true;
            probe.count = 7;
            const bool parsed = SliceMap::parse (text, probe);
            const auto compiled = SliceMap::compile (text);
            allOff = allOff && ! parsed && ! probe.on && probe.count == 0 && compiled != nullptr && ! compiled->on && compiled->count == 0
                     && compiled->sliceFor (36) == -1;
        }
        check (allOff, "12 malformed / empty / descending documents: parse false, compile = a map that is OFF");
        check (! SliceMap::compile (SliceMap::defaultJson())->on, "the default document is off");
    }

    //==========================================================================
    // 2. backend document handling
    //==========================================================================
    void testBackendDocument()
    {
        std::printf ("\n[slice] 2. backend: verbatim storage, getUiStateJson, malformed = off\n");

        Rig rig (makeNoise (kFrames, 1, "noise"));
        auto& be = rig.proc.getBackendImpl();

        {
            const auto state = juce::JSON::parse (be.getUiStateJson());
            const auto sm = state.getProperty ("sliceMap", juce::var());
            check (sm.getDynamicObject() != nullptr && ! (bool) sm.getProperty ("on", true), "never set: getUiStateJson serves sliceMap {on:false}");
        }

        const auto edges = legalEdges6();
        const juce::String doc = mapJson (true, 40, edges);
        const uint32_t before = be.revision (WarprUiBackend::Revision::uiState);
        be.setSliceMapJson (doc);
        check (be.copyModel().sliceMapJson == doc, "the model stores the document verbatim");
        check (be.revision (WarprUiBackend::Revision::uiState) == before, "a UI edit does not bump Revision::uiState (no echo)");

        {
            const auto state = juce::JSON::parse (be.getUiStateJson());
            const auto sm = state.getProperty ("sliceMap", juce::var());
            check (juce::JSON::toString (sm, true) == juce::JSON::toString (juce::JSON::parse (doc), true),
                   "getUiStateJson serves exactly the document that was sent");
        }

        rig.block();
        auto a = rig.play (45, 8);
        check (a.peak() > 0.01f, "the published map is on: its slice 5 (note 45) sounds");

        be.setSliceMapJson ("this is not json");
        rig.block();
        check (be.copyModel().sliceMapJson == "this is not json", "a malformed document is still stored verbatim");
        {
            const auto state = juce::JSON::parse (be.getUiStateJson());
            const auto sm = state.getProperty ("sliceMap", juce::var());
            check (sm.getDynamicObject() != nullptr && ! (bool) sm.getProperty ("on", true), "... and served as the default (off) document");
        }
        for (int i = 0; i < 40; ++i)
            rig.block();
        a = rig.play (60, 8);
        check (a.peak() > 0.01f, "malformed = slicing off: an ordinary note (60) plays again");
    }

    //==========================================================================
    // 3 + 4. region latch exact, unpitched
    //==========================================================================
    void testRegionLatch()
    {
        std::printf ("\n[slice] 3. note -> slice region latch is EXACT (vs the engine playing the full sample)\n");

        const auto edges = legalEdges6();
        auto sample = makeNoise (kFrames, 7, "noise");

        Rig sliced (sample), ref (sample);
        sliced.setSlices (mapJson (true, 36, edges));
        setParam (ref.proc, "loopMode", 2.0f);                             // One-Shot

        bool allExact = true, allAudible = true, allEnded = true, readback = true;
        std::vector<Audio> perSlice;

        for (int k = 0; k < 6; ++k)
        {
            setParam (ref.proc, "sampleStart", edges[(size_t) k]);
            setParam (ref.proc, "sampleEnd", edges[(size_t) k + 1]);
            readback = readback && rawOf (ref.proc, "sampleStart") == edges[(size_t) k] && rawOf (ref.proc, "sampleEnd") == edges[(size_t) k + 1];

            const Audio a = sliced.play (36 + k, kRenderBlocks);
            const Audio b = ref.play (60, kRenderBlocks);
            const bool same = identical (a, b);
            allExact = allExact && same;
            allAudible = allAudible && a.peak() > 0.05f;
            allEnded = allEnded && a.peakFrom (a.l.size() - (size_t) (8 * kBlock)) < 1.0e-3f && sliced.proc.getNumActiveVoices() == 0;
            std::printf ("        slice %d (note %d): peak %.3f, bit-exact vs reference: %s\n", k, 36 + k, a.peak(), same ? "yes" : "NO");
            perSlice.push_back (a);
        }

        check (readback, "reference parameters read back exactly the slice edges");
        check (allExact, "all 6 slices: bit-exact vs the full-sample engine with start/end = the slice, One-Shot, root note");
        check (allAudible, "all 6 slices are audible");
        check (allEnded, "every slice ends by itself: silent tail, no voice left");
        check (! identical (perSlice[0], perSlice[1]) && ! identical (perSlice[2], perSlice[3]), "different slices sound different");

        // the playMode / loopMode parameters do not influence a slice voice
        // (compared against a fresh engine with the identical history: the master stage keeps tiny filter state between notes)
        Audio base;
        {
            Rig r0 (sample);
            r0.setSlices (mapJson (true, 36, edges));
            base = r0.play (38, kRenderBlocks);
        }
        check (base.peak() > 0.05f && ! identical (base, perSlice[0]), "baseline: slice 2 of a fresh engine");
        struct Variant { const char* what; const char* id; float value; };
        bool paramsIgnored = true;

        for (const Variant& v : { Variant { "loopMode Forward Loop", "loopMode", 0.0f }, Variant { "loopMode Ping-Pong", "loopMode", 1.0f },
                                  Variant { "playMode Granular", "playMode", 1.0f } })
        {
            Rig r (sample);
            r.setSlices (mapJson (true, 36, edges));
            setParam (r.proc, v.id, v.value);
            const Audio a = r.play (38, kRenderBlocks);
            const bool same = identical (a, base);
            paramsIgnored = paramsIgnored && same;
            std::printf ("        %s: %s\n", v.what, same ? "identical" : "DIFFERS");
        }
        check (paramsIgnored, "slice voices are One-Shot + Classic whatever loopMode / playMode say (bit-exact)");

        // the global region does not move a slice
        {
            Rig r (sample);
            r.setSlices (mapJson (true, 36, edges));
            setParam (r.proc, "sampleStart", 0.6f);
            setParam (r.proc, "sampleEnd", 0.7f);
            const Audio a = r.play (38, kRenderBlocks);
            check (identical (a, base), "the global sampleStart / sampleEnd do not affect a slice voice");
        }
    }

    void testUnpitched()
    {
        std::printf ("\n[slice] 4. unpitched: the note selects the slice, not the pitch\n");

        const auto edges = legalEdges6();
        auto sine = makeSine (kFrames, 1000.0, 0.5f);
        Rig rig (sine);
        setParam (rig.proc, "unisonVoices", 1.0f);
        rig.setSlices (mapJson (true, 36, edges));

        const int n = 1800;                                                // inside the shortest slice (3000 frames), clear of its end fade
        const int skip = 3 * kBlock;                                       // past the attack

        const Audio a0 = rig.play (36, 20);
        const Audio a5 = rig.play (41, 40);
        const double hz0 = estimateHz (a0.l.data() + skip, n), hz5 = estimateHz (a5.l.data() + skip, n);
        std::printf("        slice 0 @ note 36: %.1f Hz, slice 5 @ note 41: %.1f Hz\n", hz0, hz5);
        check (std::abs (hz0 - 1000.0) < 8.0 && std::abs (hz5 - 1000.0) < 8.0, "slice 0 (note 36) and slice 5 (note 41) both read at the original 1000 Hz");

        setParam (rig.proc, "transpose", 12.0f);
        for (int i = 0; i < 20; ++i)
            rig.block();
        const Audio t3 = rig.play (39, 20);
        const double hzT = estimateHz (t3.l.data() + 2 * kBlock, 600);      // +12 st reads twice as fast: the slice is 1500 samples long
        std::printf("        slice 3 @ note 39 with transpose +12: %.1f Hz\n", hzT);
        check (std::abs (hzT - 2000.0) < 16.0, "transpose still applies to a slice voice (+12 st = 2000 Hz)");
        setParam (rig.proc, "transpose", 0.0f);

        // control: an unsliced note is pitched
        Rig plain (sine);
        setParam (plain.proc, "unisonVoices", 1.0f);
        const Audio p = plain.play (72, 20);
        const double hzP = estimateHz (p.l.data() + skip, n);
        std::printf("        control: slicing off, note 72: %.1f Hz\n", hzP);
        check (std::abs (hzP - 2000.0) < 16.0, "control: with slicing off note 72 plays an octave up (2000 Hz)");

        // a root note change keeps the slice voice unpitched too
        setParam (rig.proc, "rootNote", 48.0f);
        for (int i = 0; i < 20; ++i)
            rig.block();
        const Audio r = rig.play (37, 20);
        check (std::abs (estimateHz (r.l.data() + skip, n) - 1000.0) < 8.0, "rootNote 48: a slice voice is still at the original rate");
    }

    //==========================================================================
    // 5. outside range / off
    //==========================================================================
    void testOutsideRange()
    {
        std::printf ("\n[slice] 5. notes outside the slice keyboard are silent\n");

        const auto edges = legalEdges6();
        Rig rig (makeNoise (kFrames, 3, "noise"));
        rig.setSlices (mapJson (true, 36, edges));

        bool silent = true;
        for (int note : { 0, 35, 42, 60, 127 })
        {
            const Audio a = rig.play (note, 6);
            silent = silent && a.peak() == 0.0f && rig.proc.getNumActiveVoices() == 0;
        }
        check (silent, "notes 0, 35, 42 (= base + N), 60, 127 produce exactly nothing and start no voice");

        const Audio edgeLo = rig.play (36, 12), edgeHi = rig.play (41, 12);
        check (edgeLo.peak() > 0.05f && edgeHi.peak() > 0.05f, "the first (36) and the last (41) mapped notes sound");

        // a base near the top of the keyboard: slices beyond note 127 are unreachable, the rest work
        rig.setSlices (mapJson (true, 125, edges));
        const Audio top = rig.play (127, 20), under = rig.play (124, 6);
        std::printf ("        (baseNote 125: note 127 peak %.6f, note 124 peak %.6f)\n", (double) top.peak(), (double) under.peak());
        // MEASURED 1.04e-3 on note 124: the master DC blocker rings down from the PRECEDING render,
        // which hit the limiter ceiling (0.9886) - the loudest preceding render in this suite.  The
        // semantic claim is "no voice starts"; the level gate allows that residue.
        check (top.peak() > 0.05f && under.peak() < 2.5e-3f && rig.proc.getNumActiveVoices() == 0,
               "baseNote 125: note 127 = slice 2 sounds, note 124 starts no voice (DC residue only)");

        rig.setSlices (mapJson (false, 36, edges));
        const Audio off = rig.play (60, 12);
        check (off.peak() > 0.05f, "\"on\":false: ordinary pitched playback");
        check (rig.finite, "all output finite");
    }

    //==========================================================================
    // 6. one-shot gates
    //==========================================================================
    void testTriggerGates()
    {
        std::printf ("\n[slice] 6. note-off / pedal ignored, auto-release, voice freed\n");

        const auto edges = legalEdges6();
        auto sample = makeNoise (kFrames, 5, "noise");

        auto freedAt = [&] (Rig& r, int note, int blocks, int offAt, bool pedal, Audio& out)
        {
            int freed = -1;

            for (int b = 0; b < blocks; ++b)
            {
                juce::MidiBuffer m;

                if (b == 0)
                {
                    if (pedal)
                        m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
                    m.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
                }
                else if (b == offAt)
                {
                    m.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                }
                else if (pedal && b == offAt + 1)
                {
                    m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
                }

                r.block (m, &out);

                if (freed < 0 && r.proc.getNumActiveVoices() == 0)
                    freed = b;
            }

            return freed;
        };

        // slice 1 = 3000 frames (11.7 blocks)
        Rig held (sample), tapped (sample), pedaled (sample);
        held.setSlices (mapJson (true, 36, edges));
        tapped.setSlices (mapJson (true, 36, edges));
        pedaled.setSlices (mapJson (true, 36, edges));

        Audio aHeld, aTap, aPedal;
        const int fHeld = freedAt (held, 37, 30, 1000, false, aHeld);       // the key is never released inside the render
        const int fTap = freedAt (tapped, 37, 30, 1, false, aTap);          // released one block after the strike
        const int fPedal = freedAt (pedaled, 37, 30, 1, true, aPedal);      // pedal down, key-up, pedal-up

        std::printf("        voice freed after block: held %d, key-up %d, pedal %d (slice = 11.7 blocks)\n", fHeld, fTap, fPedal);
        check (identical (aHeld, aTap), "a key-up during the slice changes nothing (bit-exact vs the held key)");
        check (identical (aHeld, aPedal), "sustain pedal down / key-up / pedal-up changes nothing (bit-exact)");
        check (fHeld >= 11 && fHeld <= 14 && fTap == fHeld && fPedal == fHeld, "the voice plays the whole slice, auto-releases at its end and is freed (11..14 blocks)");

        // a fresh tap of a slice ends by itself even if the host never sends a note-off
        Rig noOff (sample);
        noOff.setSlices (mapJson (true, 36, edges));
        Audio n;
        const int f = freedAt (noOff, 41, 50, 1000, false, n);
        check (f >= 23 && f <= 26, "slice 5 (6000 frames) without any note-off is freed after ~24 blocks");

        // CC123 (all notes off) is a panic: it still releases a slice voice
        Rig panic (sample);
        setParam (panic.proc, "ampRelease", 10.0f);                        // short release: the panic frees the voice long before the slice ends
        panic.setSlices (mapJson (true, 36, edges));
        Audio pa;
        panic.block (noteOnEv (41), &pa);
        panic.block (juce::MidiBuffer(), &pa);
        panic.block (ccEv (123, 0), &pa);
        for (int i = 0; i < 12; ++i)
            panic.block (juce::MidiBuffer(), &pa);
        check (panic.proc.getNumActiveVoices() == 0, "CC123 still releases a slice voice (panic path)");
    }

    //==========================================================================
    // 7. map replacement mid-note
    //==========================================================================
    void testReplacementMidNote()
    {
        std::printf ("\n[slice] 7. a map replacement mid-note does not touch sounding voices\n");

        const auto edges = legalEdges6();
        auto sample = makeNoise (kFrames, 9, "noise");

        Rig steady (sample);
        steady.setSlices (mapJson (true, 36, edges));
        const Audio base = steady.play (38, kRenderBlocks);                  // slice 2

        auto withReplacement = [&] (const juce::String& replacement)
        {
            Rig r (sample);
            r.setSlices (mapJson (true, 36, edges));
            Audio a;

            for (int b = 0; b < kRenderBlocks; ++b)
            {
                if (b == 3)
                    r.proc.getBackendImpl().setSliceMapJson (replacement);          // message thread, between blocks

                r.block (b == 0 ? noteOnEv (38) : juce::MidiBuffer(), &a);
            }

            return std::make_pair (a, r.proc.getNumActiveVoices());
        };

        const auto toOff = withReplacement (mapJson (false, 36, edges));
        const auto toShifted = withReplacement (mapJson (true, 60, { 0.0f, 0.5f, 1.0f }));
        const auto toMalformed = withReplacement ("garbage");

        check (identical (toOff.first, base) && toOff.second == 0, "map turned OFF mid-note: the voice finishes its latched slice (bit-exact)");
        check (identical (toShifted.first, base) && toShifted.second == 0, "map replaced by another layout mid-note: bit-exact");
        check (identical (toMalformed.first, base) && toMalformed.second == 0, "map replaced by garbage mid-note: bit-exact");

        // after the replacement the NEW layout rules the next note-ons
        Rig r (sample);
        r.setSlices (mapJson (true, 36, edges));
        r.setSlices (mapJson (true, 60, { 0.0f, 0.5f, 1.0f }));
        const Audio oldNote = r.play (38, 8);
        const Audio newNote = r.play (60, 12);
        check (oldNote.peak() == 0.0f && newNote.peak() > 0.05f, "next note-ons use the new map (38 now silent, 60 = new slice 0 sounds)");
    }

    //==========================================================================
    // 8. mono / legato
    //==========================================================================
    void testMonoLegato()
    {
        std::printf ("\n[slice] 8. Mono / Legato: a slice note never retargets the pitch\n");

        const auto edges = legalEdges6();
        auto sine = makeSine (kFrames, 1000.0, 0.5f);

        for (int mode = 1; mode <= 2; ++mode)
        {
            Rig rig (sine);
            setParam (rig.proc, "unisonVoices", 1.0f);
            setParam (rig.proc, "voiceMode", (float) mode);
            setParam (rig.proc, "glideTime", 400.0f);                       // a retarget would glide audibly
            rig.setSlices (mapJson (true, 36, edges));

            Audio a;
            rig.block (noteOnEv (36), &a);                                  // slice 0, key held
            rig.block (juce::MidiBuffer(), &a);
            rig.block (noteOnEv (39), &a);                                  // slice 3 overlaps (legato would retarget)
            for (int i = 0; i < 14; ++i)
                rig.block (juce::MidiBuffer(), &a);

            const size_t from = (size_t) (4 * kBlock);                      // after the 3 ms ghost of slice 0
            const double hz = estimateHz (a.l.data() + from, 2048);
            std::printf("        voiceMode %d: slice 3 sounds at %.1f Hz\n", mode, hz);
            check (std::abs (hz - 1000.0) < 8.0, mode == 1 ? "Mono: second slice note starts a fresh unpitched voice"
                                                           : "Legato: overlapping slice notes do not retarget / glide");
            check (rig.finite, "finite");
        }
    }

    //==========================================================================
    // 9. state / preset persistence
    //==========================================================================
    void testPersistence()
    {
        std::printf ("\n[slice] 9. uiState round trip, presets, state without a map\n");

        const auto edges = legalEdges6();
        auto sample = makeNoise (kFrames, 11, "noise");
        const juce::String doc = mapJson (true, 48, edges);

        Rig a (sample);
        a.setSlices (doc);
        const Audio audioA = a.play (48 + 3, kRenderBlocks);
        check (audioA.peak() > 0.05f, "source engine: slice 3 at base 48 (note 51) sounds");

        const auto block = a.proc.captureState();
        Rig b;
        const auto outcome = b.proc.loadStateBlock (block.getData(), block.getSize());
        check (outcome == warpr::state::LoadOutcome::loaded, "state loads");
        check (b.proc.getBackendImpl().copyModel().sliceMapJson == doc, "the loaded model holds the identical slice document");
        check (b.proc.getBackendImpl().getUiStateJson() == a.proc.getBackendImpl().getUiStateJson(), "getUiStateJson is identical on the fresh processor");
        check (b.proc.getBackendImpl().revision (WarprUiBackend::Revision::uiState) > 0, "adoptModel bumped Revision::uiState");

        b.block();
        const Audio audioB = b.play (51, kRenderBlocks);
        check (identical (audioA, audioB), "the fresh processor plays identical audio for the same slice note (state restored slicing)");
        const Audio silentB = b.play (60, 6);
        check (silentB.peak() < 1.0e-3f, "... and an unmapped note is silent there too");

        // preset file
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("warpr_slice_test" + warpr::PresetManager::extension());
        file.deleteFile();
        const bool saved = a.proc.getBackendImpl().savePresetToFile (file);
        check (saved && file.existsAsFile(), "preset saved");

        Rig c;
        const bool loaded = c.proc.getBackendImpl().loadPresetFile (file);
        check (loaded && c.proc.getBackendImpl().copyModel().sliceMapJson == doc, "preset load restores the identical document");
        c.block();
        const Audio audioC = c.play (51, kRenderBlocks);
        check (identical (audioA, audioC), "preset: identical slice audio");
        file.deleteFile();

        // a state saved without any slice map turns slicing off again
        Rig plainSrc (sample);
        const auto plainBlock = plainSrc.proc.captureState();
        check (plainSrc.proc.getBackendImpl().copyModel().sliceMapJson.isEmpty(), "a never-sliced processor stores no map");
        b.proc.loadStateBlock (plainBlock.getData(), plainBlock.getSize());
        b.block();
        const Audio afterPlain = b.play (60, 12);
        check (b.proc.getBackendImpl().copyModel().sliceMapJson.isEmpty() && afterPlain.peak() > 0.05f,
               "loading a state without a map clears slicing: note 60 plays pitched again");

        check (a.finite && b.finite && c.finite, "finite");
    }

    //==========================================================================
    // 10. determinism
    //==========================================================================
    void testDeterminism()
    {
        std::printf ("\n[slice] 10. determinism\n");

        const auto edges = legalEdges6();
        auto sample = makeNoise (kFrames, 13, "noise");

        auto sequence = [&] (int voiceMode)
        {
            Rig r (sample);
            setParam (r.proc, "voiceMode", (float) voiceMode);
            r.setSlices (mapJson (true, 36, edges));
            Audio a;

            for (int b = 0; b < 60; ++b)
            {
                juce::MidiBuffer m;

                if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
                if (b == 2) m.addEvent (juce::MidiMessage::noteOn (1, 39, 0.5f), 17);
                if (b == 3) m.addEvent (juce::MidiMessage::noteOn (1, 41, 0.8f), 100);
                if (b == 4) m.addEvent (juce::MidiMessage::noteOff (1, 36), 3);
                if (b == 5) m.addEvent (juce::MidiMessage::noteOn (1, 36, 0.9f), 0);      // same slice again: retrigger
                if (b == 6) m.addEvent (juce::MidiMessage::noteOn (1, 50, 1.0f), 5);      // outside
                r.block (m, &a);
            }

            return a;
        };

        bool same = true, audible = true;
        for (int mode = 0; mode <= 2; ++mode)
        {
            const Audio first = sequence (mode), second = sequence (mode);
            same = same && identical (first, second);
            audible = audible && first.peak() > 0.05f;
        }
        check (audible, "Poly / Mono / Legato slice sequences are audible");
        check (same, "two fresh engines render bit-identical audio (Poly / Mono / Legato)");
    }

    //==========================================================================
    // 11. allocations
    //==========================================================================
    void testAllocations()
    {
        std::printf ("\n[slice] 11. zero allocations on the audio thread\n");

        const auto edges = legalEdges6();
        Rig rig (makeNoise (kFrames, 17, "noise"));
        rig.setSlices (mapJson (true, 36, edges));

        juce::AudioBuffer<float> buf (2, kBlock);
        std::vector<juce::MidiBuffer> blocks (60);

        for (int note : { 36, 37, 38, 39, 40, 41, 20, 80 })
            blocks[(size_t) (note % 4)].addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), note % 7);
        blocks[10].addEvent (juce::MidiMessage::noteOff (1, 36), 0);
        blocks[12].addEvent (juce::MidiMessage::noteOn (1, 41, 1.0f), 0);

        std::size_t allocs = 0;

        {
            warpr::tests::ScopedAllocCount counter;

            for (auto& m : blocks)
            {
                buf.clear();
                rig.proc.processBlock (buf, m);
            }

            allocs = counter.count();
        }
        check (allocs == 0, "60 blocks with slice note-ons, note-offs, out-of-range notes and voice ends: zero allocations");

        // a map published between blocks is adopted on the audio thread without allocating
        rig.proc.getBackendImpl().setSliceMapJson (mapJson (true, 40, { 0.0f, 0.5f, 1.0f }));
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 41, 1.0f), 0);

        {
            warpr::tests::ScopedAllocCount counter;

            for (int i = 0; i < 30; ++i)
            {
                buf.clear();
                rig.proc.processBlock (buf, m);
                m.clear();
            }

            allocs = counter.count();
        }
        check (allocs == 0, "adopting a replaced map and playing its slice: zero allocations");
        rig.proc.drainRetired();
    }

    //==========================================================================
    // 12. auto-slicer
    //==========================================================================
    juce::AudioBuffer<float> clickTrain (int frames, const std::vector<std::pair<int, float>>& clicks)
    {
        juce::AudioBuffer<float> b (1, frames);
        juce::Random rng (99);

        for (int i = 0; i < frames; ++i)
            b.setSample (0, i, 0.001f * (rng.nextFloat() * 2.0f - 1.0f));            // low noise floor

        for (const auto& c : clicks)
            for (int k = 0; k < 240 && c.first + k < frames; ++k)
                b.setSample (0, c.first + k, b.getSample (0, c.first + k) + c.second * (float) std::exp (-(double) k / 48.0));

        return b;
    }

    void testAutoSlicer()
    {
        std::printf ("\n[slice] 12. computeSliceEdges / autoSliceEdges\n");
        using warpr::autoSliceEdges;

        const int n = 115200;                                                      // 2.4 s @ 48 kHz
        const auto train = clickTrain (n, { { 19200, 0.6f }, { 38400, 0.9f }, { 57600, 0.7f }, { 76800, 0.8f } });
        const float* x = train.getReadPointer (0);

        auto show = [&] (const char* what, const std::vector<double>& e)
        {
            std::printf ("        %s:", what);
            for (double v : e)
                std::printf (" %.0f", v * (double) n);
            std::printf ("  (frames)\n");
        };

        // equal division
        {
            const auto e = autoSliceEdges (x, nullptr, n, kFs, 0.25, 0.75, 4, false);
            check (e.size() == 5 && e.front() == 0.25 && e.back() == 0.75 && e[1] == 0.375 && e[2] == 0.5 && e[3] == 0.625,
                   "equal division of [0.25, 0.75] into 4: exact edges, front/back exact");
            check (autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 1, true).size() == 2, "count 1 = region edges only");
            check (autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 500, false).size() == 65, "count clamps to 64 slices");
            check (autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 0, false).size() == 2, "count clamps to >= 1");
        }

        // transients: 4 slices -> the 3 strongest of the 4 clicks (0.6 @ 19200 is the weakest and loses)
        {
            const auto e = autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 4, true);
            show ("count 4, strongest 3 of 4 clicks", e);
            const bool shapeOk = e.size() == 5 && e.front() == 0.0 && e.back() == 1.0;
            check (shapeOk && std::abs (e[1] * n - 38400.0) <= 2.0 && std::abs (e[2] * n - 57600.0) <= 2.0 && std::abs (e[3] * n - 76800.0) <= 2.0,
                   "the 3 strongest onsets (0.9 @ 38400, 0.7 @ 57600, 0.8 @ 76800) become edges within 2 frames");
            check (e == autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 4, true), "deterministic (repeat call identical)");
        }

        // all four clicks as edges
        {
            const auto e = autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 5, true);
            show ("count 5", e);
            check (e.size() == 6 && std::abs (e[1] * n - 19200.0) <= 2.0 && std::abs (e[2] * n - 38400.0) <= 2.0
                       && std::abs (e[3] * n - 57600.0) <= 2.0 && std::abs (e[4] * n - 76800.0) <= 2.0,
                   "count 5 = all four clicks, ascending");
        }

        // fewer onsets than requested: `count` is a CAP - the result holds every real onset and
        // stops there (no grid fallback in transient mode; owner request 2026-11-16)
        {
            const auto e = autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 8, true);
            show ("count 8 (cap) on 4 clicks", e);
            check (e.size() == 6 && std::abs (e[1] * n - 19200.0) <= 2.0 && std::abs (e[4] * n - 76800.0) <= 2.0,
                   "count 8 with only 4 onsets: returns exactly the 4 real onsets (cap, no grid fallback)");

            juce::AudioBuffer<float> silence (1, n);
            silence.clear();
            check (autoSliceEdges (silence.getReadPointer (0), nullptr, n, kFs, 0.0, 1.0, 4, true) == autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 4, false),
                   "digital silence: equal division");

            // REVIEW FIX 2026-11-16: a one-shot whose ONLY onset sits inside the 30 ms start guard
            // must yield ONE slice {s, e} - never a count-sized grid (the removed fallback leaked
            // through the chosen-empty path before).
            juce::AudioBuffer<float> kick (1, n);
            kick.clear();
            const int kickPos = (int) std::lround (0.020 * kFs);           // 20 ms: inside the gap
            for (int i = 0; i < 48; ++i)
                kick.setSample (0, kickPos + i, 0.9f * (1.0f - (float) i / 48.0f));
            const auto ke = autoSliceEdges (kick.getReadPointer (0), nullptr, n, kFs, 0.0, 1.0, 8, true);
            show ("lone 20 ms kick, count 8", ke);
            check (ke.size() == 2 && juce::exactlyEqual (ke.front(), 0.0) && juce::exactlyEqual (ke.back(), 1.0),
                   "a lone onset inside the 30 ms guard yields ONE slice, not a grid");
        }

        // 30 ms minimum gap: a strong click 20 ms after the strongest is skipped
        {
            const auto gapTrain = clickTrain (n, { { 19200, 1.0f }, { 19200 + 960, 0.9f }, { 57600, 0.5f } });
            const auto e = autoSliceEdges (gapTrain.getReadPointer (0), nullptr, n, kFs, 0.0, 1.0, 3, true);
            show ("count 3, clicks 20 ms apart + one weaker", e);
            check (e.size() == 4 && std::abs (e[1] * n - 19200.0) <= 2.0 && std::abs (e[2] * n - 57600.0) <= 2.0,
                   "a click 20 ms after a chosen onset is skipped (30 ms gap); the next-strongest distant one is used");

            // a click within 30 ms of the region start never becomes an edge
            const auto nearStart = clickTrain (n, { { 700, 1.0f }, { 38400, 0.5f } });
            const auto e2 = autoSliceEdges (nearStart.getReadPointer (0), nullptr, n, kFs, 0.0, 1.0, 2, true);
            show ("count 2, strong click 14 ms after the region start", e2);
            check (e2.size() == 3 && std::abs (e2[1] * n - 38400.0) <= 2.0, "a click closer than 30 ms to the region start is not an edge");
        }

        // region restriction
        {
            const auto e = autoSliceEdges (x, nullptr, n, kFs, 0.25, 0.75, 3, true);
            show ("region [0.25, 0.75], count 3", e);
            check (e.size() == 4 && e.front() == 0.25 && e.back() == 0.75 && std::abs (e[1] * n - 38400.0) <= 2.0 && std::abs (e[2] * n - 76800.0) <= 2.0,
                   "only onsets inside the region count (the 2 strongest of 0.9 / 0.7 / 0.8), edges stay in the region");
        }

        // stereo input = the mono mix
        {
            juce::AudioBuffer<float> st (2, n);
            st.copyFrom (0, 0, train, 0, 0, n);
            st.copyFrom (1, 0, train, 0, 0, n);
            check (autoSliceEdges (st.getReadPointer (0), st.getReadPointer (1), n, kFs, 0.0, 1.0, 4, true)
                       == autoSliceEdges (x, nullptr, n, kFs, 0.0, 1.0, 4, true),
                   "identical stereo channels give the mono result");
        }

        // the backend path (message thread): current sample + current sampleStart / sampleEnd
        {
            Rig rig (warpr::SampleData::create (train, kFs, "clicks", false, false));
            std::vector<double> e;
            auto& be = rig.proc.getBackendImpl();
            check (be.computeSliceEdges (4, true, e) && e.size() == 5 && e.front() == 0.0 && e.back() == 1.0
                       && std::abs (e[1] * n - 38400.0) <= 2.0 && std::abs (e[3] * n - 76800.0) <= 2.0,
                   "backend computeSliceEdges(4, transients) on the published sample");

            const float s = legal (rig.proc, "sampleStart", 0.25f), t = legal (rig.proc, "sampleEnd", 0.75f);
            check (be.computeSliceEdges (3, false, e) && e.size() == 4 && e.front() == (double) s && e.back() == (double) t,
                   "backend equal division follows the sampleStart / sampleEnd parameters");
            check (be.computeSliceEdges (3, true, e) && e == autoSliceEdges (x, nullptr, n, kFs, (double) s, (double) t, 3, true),
                   "backend transient result == the pure function on the same region");

            // feed the result straight into the engine: the slices of an auto-sliced loop are playable
            setParam (rig.proc, "sampleStart", 0.0f);
            setParam (rig.proc, "sampleEnd", 1.0f);
            be.computeSliceEdges (4, true, e);
            std::vector<float> f;
            for (double v : e)
                f.push_back ((float) v);
            rig.setSlices (mapJson (true, 36, f));
            const Audio a = rig.play (37, 30);
            check (a.peak() > 0.05f && rig.finite, "an auto-sliced map plays (note 37 = slice 1)");
        }

        // default backend (no sample support) contract
        {
            struct Stub : WarprUiBackend
            {
                int getPlayheads (WarprVizFrame::Voice*, int) override { return 0; }
                WarprVizFifo& vizFifo() override { return fifo; }
                uint32_t revision (Revision) const override { return 0; }
                juce::String getUiStateJson() override { return {}; }
                void setLfoCount (int) override {}
                void setLfoShapeJson (int, const juce::String&) override {}
                void setModMatrixJson (const juce::String&) override {}
                void setShaperCurveJson (const juce::String&, const juce::String&) override {}
                juce::String getShaperCurveJson (const juce::String&) override { return {}; }
                void shaperReset (const juce::String&) override {}
                void shaperNormalize (const juce::String&) override {}
                void loadSampleFile (const juce::File&) override {}
                WarprSampleDisplay getSampleDisplay() override { return {}; }
                void computeFilterResponse (float*, int) override {}
                juce::String getPresetName() override { return {}; }
                void presetStep (int) override {}
                void presetSaveDialog() override {}
                void presetLoadDialog() override {}
                void openSettings() override {}
                WarprVizFifo fifo;
            } stub;

            std::vector<double> e { 1.0 };
            stub.setSliceMapJson ("{}");
            check (! stub.computeSliceEdges (4, true, e) && e.empty(), "a backend without slice support reports false and clears the output");
        }
    }
}

//==============================================================================
    //==========================================================================
    // POS-KEY grain mode (owner request 2026-11-16): MIDI notes pick the grain POSITION
    // (C1 = region start, C6 = end), unpitched; off by default; persists via uiState.
    //==========================================================================
    static void testGrainPosKey()
    {
        std::printf ("\n[pos-key] granular: notes pick the position, unpitched\n");

        auto grainCenter = [] (Rig& rig, int note) -> double
        {
            juce::MidiBuffer on;
            on.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
            rig.block (on);
            for (int b = 0; b < 30; ++b) rig.block();                      // let the cloud settle

            float gp[32], gl[32];
            const int n = rig.proc.getSynth().readGrains (gp, gl, 32);
            double sum = 0.0;
            for (int i = 0; i < n; ++i) sum += (double) gp[i];
            const double c = n > 0 ? sum / n : -1.0;

            juce::MidiBuffer off;
            off.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            rig.block (off);
            for (int b = 0; b < 40; ++b) rig.block();                      // release + free
            return c;
        };

        Rig rig (makeNoise ((int) kFs * 2, 21, "Long"));
        setParam (rig.proc, "playMode", 1.0f);                             // Granular
        setParam (rig.proc, "grainMode", 2.0f);                            // Cloud (many live grains)
        rig.block();

        const double offC3 = grainCenter (rig, 66);                        // flag OFF: position knob (0) rules
        rig.proc.getSynth().setGrainPosKey (true);
        const double keyLow  = grainCenter (rig, 38);                      // 2 keys above C1 -> ~0.033
        const double keyMid  = grainCenter (rig, 66);                      // 30 keys above C1 -> 0.5
        const double keyHigh = grainCenter (rig, 94);                      // ~0.967
        rig.proc.getSynth().setGrainPosKey (false);

        std::printf ("        grain cloud centers: off %.3f, key-low %.3f, key-mid %.3f, key-high %.3f\n",
                     offC3, keyLow, keyMid, keyHigh);
        // centers carry grain-length / spread scatter, so the gates are coarse but ordered
        check (offC3 >= 0.0 && offC3 < 0.25, "flag off: the cloud sits at the POSITION knob (start)");
        check (keyLow >= 0.0 && keyLow < 0.25, "POS-KEY: a key near C1 puts the cloud near the start");
        check (keyMid > 0.30 && keyMid < 0.70, "POS-KEY: 30 keys up puts the cloud mid-sample");
        check (keyHigh > 0.70, "POS-KEY: a key near C6 puts the cloud near the end");

        {   // mono/legato: a POS-KEY voice is never RETARGETED (that would glide root -> key, i.e.
            // detune an unpitched voice, and never move the cloud): the second key starts a fresh
            // voice whose cloud sits at ITS position.
            Rig mono (makeNoise ((int) kFs * 2, 23, "Long"));
            setParam (mono.proc, "playMode", 1.0f);
            setParam (mono.proc, "grainMode", 2.0f);
            setParam (mono.proc, "voiceMode", 2.0f);                       // Legato
            mono.proc.getSynth().setGrainPosKey (true);
            mono.block();

            juce::MidiBuffer a;
            a.addEvent (juce::MidiMessage::noteOn (1, 38, 0.8f), 0);       // near the start
            mono.block (a);
            for (int b = 0; b < 30; ++b) mono.block();

            juce::MidiBuffer b2;
            b2.addEvent (juce::MidiMessage::noteOn (1, 90, 0.8f), 0);      // overlap: legato gesture
            mono.block (b2);
            for (int b = 0; b < 30; ++b) mono.block();

            float gp[32], gl[32];
            const int n = mono.proc.getSynth().readGrains (gp, gl, 32);
            double sum = 0.0;
            for (int i = 0; i < n; ++i) sum += (double) gp[i];
            const double c = n > 0 ? sum / n : -1.0;
            std::printf ("        legato re-position: cloud center %.3f after the second key\n", c);
            check (c > 0.5, "mono/legato POS-KEY: the second key RE-POSITIONS the cloud (fresh voice, no retarget)");
            mono.proc.getSynth().setGrainPosKey (false);
        }

        // persistence: the flag travels inside uiState (state save -> fresh processor -> restored)
        rig.proc.getBackendImpl().setGrainPosKey (true);
        juce::MemoryBlock state;
        rig.proc.getStateInformation (state);

        Rig fresh (makeNoise ((int) kFs, 22, "Other"));
        fresh.proc.setStateInformation (state.getData(), (int) state.getSize());
        fresh.block();
        check (fresh.proc.getUiBackend().getUiStateJson().contains ("\"grainPosKey\": true")
               || fresh.proc.getUiBackend().getUiStateJson().contains ("\"grainPosKey\":true"),
               "POS-KEY survives a state save / load (uiState)");
    }

    //==========================================================================
    // One-Shot choke (owner bug report 2026-11-16): a new One-Shot tap cuts every other
    // free-running one-shot voice (no stacked playheads); chords inside 30 ms survive;
    // slice pads stay polyphonic across different notes.
    //==========================================================================
    static void testOneShotChoke()
    {
        std::printf ("\n[one-shot choke] new tap cuts the previous free-running trigger\n");

        float ph[32], lv[32];

        {   // 1. tap C3, wait ~0.5 s (released, still latched), tap D3 -> only D3 keeps sounding
            Rig rig (makeNoise ((int) kFs * 2, 11, "Long"));
            setParam (rig.proc, "loopMode", 2.0f);                                 // One-Shot
            rig.block();

            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn  (1, 60, 0.8f), 0);
            m.addEvent (juce::MidiMessage::noteOff (1, 60), 64);
            rig.block (m);
            for (int b = 0; b < 90; ++b) rig.block();                              // ~0.5 s at 256 frames
            check (rig.proc.getSynth().readPlayheads (ph, lv, 32) == 1, "the first tap free-runs after release");

            juce::MidiBuffer m2;
            m2.addEvent (juce::MidiMessage::noteOn (1, 62, 0.8f), 0);
            rig.block (m2);
            for (int b = 0; b < 4; ++b) rig.block();                               // let the 3 ms ghost fade finish
            const int n = rig.proc.getSynth().readPlayheads (ph, lv, 32);
            std::printf ("        after the second tap: %d sounding voice(s)\n", n);
            check (n == 1, "the new One-Shot tap CHOKES the previous free-running voice (one playhead)");
        }

        {   // 1b. ordering: slot 0 of readPlayheads is always the NEWEST note (the UI draws only it)
            Rig rig (makeNoise ((int) kFs * 2, 13, "Long"));
            setParam (rig.proc, "loopMode", 0.0f);                   // gated overlap (One-Shot would choke the older note)
            rig.block();

            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            rig.block (m);
            for (int b = 0; b < 60; ++b) rig.block();                              // older note ~0.33 s in

            juce::MidiBuffer m2;
            m2.addEvent (juce::MidiMessage::noteOn (1, 62, 0.8f), 0);              // both still held (gate overlap)
            rig.block (m2);
            for (int b = 0; b < 4; ++b) rig.block();

            const int n = rig.proc.getSynth().readPlayheads (ph, lv, 32);
            std::printf ("        overlap: %d voices, slot0 %.4f, slot1 %.4f\n", n, ph[0], n > 1 ? ph[1] : -1.0f);
            check (n == 2 && ph[0] < ph[1], "readPlayheads is NEWEST-FIRST (slot 0 = the just-pressed note)");
        }

        {   // 2. a chord (two notes in the same block) keeps BOTH voices
            Rig rig (makeNoise ((int) kFs * 2, 12, "Long"));
            setParam (rig.proc, "loopMode", 2.0f);
            rig.block();

            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            m.addEvent (juce::MidiMessage::noteOn (1, 64, 0.8f), 32);              // same gesture, < 30 ms apart
            rig.block (m);
            for (int b = 0; b < 20; ++b) rig.block();
            check (rig.proc.getSynth().readPlayheads (ph, lv, 32) == 2, "a One-Shot chord keeps both voices sounding");
        }
    }

    //==========================================================================
    // Review 2026-11-16: MIDI arriving in a ZERO-sample block (host transport flush) must not
    // be dropped - a lost note-off is a stuck note.
    //==========================================================================
    static void testZeroSampleBlockMidi()
    {
        std::printf ("\n[zero-block midi] a note-off in an empty block still releases the voice\n");

        Rig rig (makeNoise ((int) kFs, 31, "Long"));
        setParam (rig.proc, "loopMode", 0.0f);                       // gated semantics are the point of this test
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        rig.block (on);
        for (int b = 0; b < 8; ++b) rig.block();
        check (rig.proc.getNumActiveVoices() == 1, "the note is sounding");

        juce::AudioBuffer<float> empty (2, 0);                       // zero-sample block carrying the note-off
        juce::MidiBuffer offMidi;
        offMidi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        rig.proc.processBlock (empty, offMidi);

        for (int b = 0; b < 40; ++b) rig.block();                    // release (50 ms) + free
        check (rig.proc.getNumActiveVoices() == 0, "the zero-block note-off released and freed the voice");
    }

    //==========================================================================
    // playhead regression: a Classic voice's UI playhead must ADVANCE while it plays
    //==========================================================================
    static void testPlayheadAdvances()
    {
        std::printf ("\n[playhead] Classic voice: readPlayheads advances while the note plays\n");

        Rig rig (makeNoise ((int) kFs * 2, 1, "Long"));                                // 2 s sample
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        rig.block (on);

        float p0[32], l0[32];
        std::vector<float> seen;

        for (int blk = 0; blk < 40; ++blk)
        {
            rig.block();
            const int n = rig.proc.getSynth().readPlayheads (p0, l0, 32);
            if (blk % 8 == 0)
            {
                std::printf ("        block %2d: voices %d, pos %s\n", blk, n, n > 0 ? juce::String (p0[0], 5).toRawUTF8() : "-");
                if (n > 0) seen.push_back (p0[0]);
            }
        }
        check (seen.size() >= 4, "playheads are published while the note sounds");
        bool advancing = true;
        for (size_t k = 1; k < seen.size(); ++k) advancing = advancing && seen[k] > seen[k - 1] + 1.0e-4f;
        check (advancing, "the Classic playhead ADVANCES over time (UI regression: frozen/missing line)");
        check (seen.empty() || (seen.front() >= 0.0f && seen.back() <= 1.0f), "positions stay in 0..1");
    }

//==============================================================================
// Legato re-strike (owner bug 2026-11-17): in LEGATO + One-Shot, a SAME-note overlap must
// RETRIGGER (the latched voice would otherwise play on as if the tap never happened);
// a DIFFERENT note still legato-retargets pitch over the running sample.
//==============================================================================
static void testLegatoOneShotRestrike()
{
    std::printf ("\n[legato re-strike] One-Shot + LEGATO same-note overlap restarts the sample\n");

    Rig rig (makeNoise ((int) kFs * 2, 51, "Long"));
    setParam (rig.proc, "loopMode", 2.0f);                      // One-Shot
    setParam (rig.proc, "voiceMode", 2.0f);                     // LEGATO
    rig.block();

    juce::MidiBuffer on;
    on.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
    rig.block (on);
    for (int b = 0; b < 90; ++b) rig.block();                   // ~0.5 s in

    float ph[32], lv[32];
    rig.proc.getSynth().readPlayheads (ph, lv, 32);
    const double before = ph[0];

    juce::MidiBuffer retap;                                     // overlap: new press, old key still latched
    retap.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 0);
    rig.block (retap);
    for (int b = 0; b < 4; ++b) rig.block();                    // ghost fade done

    const int n = rig.proc.getSynth().readPlayheads (ph, lv, 32);
    std::printf ("        before retap %.3f, after %.3f (%d voice(s))\n", before, ph[0], n);
    check (before > 0.2, "the first press was well into the sample");
    check (n == 1 && ph[0] < 0.1, "a same-note legato overlap RE-STRIKES from the start");

    // a DIFFERENT note must keep the legato retarget: position CONTINUES (no restart)
    for (int b = 0; b < 40; ++b) rig.block();
    rig.proc.getSynth().readPlayheads (ph, lv, 32);
    const double mid = ph[0];
    juce::MidiBuffer up;
    up.addEvent (juce::MidiMessage::noteOn (1, 64, 0.9f), 0);
    rig.block (up);
    for (int b = 0; b < 4; ++b) rig.block();
    const int n2 = rig.proc.getSynth().readPlayheads (ph, lv, 32);
    std::printf ("        different-note overlap: pos %.3f -> %.3f (%d voice(s))\n", mid, ph[0], n2);
    check (n2 == 1 && ph[0] > mid, "a different note keeps the legato retarget (sample continues)");
}

int runPhaseD11Tests (bool full)
{
    testLegatoOneShotRestrike();
    testZeroSampleBlockMidi();
    testPlayheadAdvances();
    testOneShotChoke();
    testGrainPosKey();
    juce::ignoreUnused (full);
    failures = 0;

    std::printf ("\n=== Phase D11: slice mode ===\n");

    testMapDocument();
    testBackendDocument();
    testRegionLatch();
    testUnpitched();
    testOutsideRange();
    testTriggerGates();
    testReplacementMidNote();
    testMonoLegato();
    testPersistence();
    testDeterminism();
    testAllocations();
    testAutoSlicer();

    return failures;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
