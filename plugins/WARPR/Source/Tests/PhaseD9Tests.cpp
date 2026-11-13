/*
    VOID (WARPR) - phase D9 test block: state container, embedded sample codec, fuzz, presets, settings, regression.
    Called from RunTests.cpp main():   failures += runPhaseD9Tests (full);

    Contents (plan.md "Phase D9" + T9)
      1. frozen identifiers + container layout   magic bytes, version, XML, blob count, blob header parsed by hand
      2. codec                                   FLAC24 vs float32 choice, edge samples, bit-exact decode, name truncation
      3. round trip matrix                       16/24-bit int + float32 (+ specials), mono/stereo, 6 file rates: SampleData bit-identical
      4. params + UI state round trip            135 parameters, LFO shapes, matrix, shaper curves; default wave regenerated
      5. audio equality                          state-loaded processors render identically at 4 host sample rates
      6. forward compatibility / migration       newer container version, newer ui-state version, unknown codec, legacy blob-0 payload
      7. fuzz                                    >= 1000 seeded mutations: no crash, rejected input leaves the state untouched
      8. presets                                 20 generated presets in a temp dir, step / wrap / skip-corrupt, presetName revision
      9. revisions + automation list             bump audit, 135 parameters (20 hidden)
     10. app settings                            persistence, sanitising, budget hook
     11. host-state size record                  printed
     12. soak-style determinism                  MIDI storm: two fresh runs and a run with state capture between blocks are bit-identical

    Hand-derived gates carry a comment; measured numbers are printed.  Processors are heap-allocated (64 MB stack note in CMakeLists).
*/

#include "PluginProcessor.h"
#include "Parameters/ParamTable.h"
#include "State/AppSettings.h"
#include "State/PresetManager.h"
#include "State/SampleCodec.h"
#include "State/StateIO.h"
#include "Tests/AllocHook.h"
#include "Tests/OfflineRender.h"
#include "Tests/Scanners.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")

int runPhaseD9Tests (bool full);

namespace
{
    int failures = 0;
    int printedDetails = 0;

    void check (bool cond, const char* name)
    {
        std::printf ("  %s  %s\n", cond ? "PASS" : "FAIL", name);
        if (! cond) ++failures;
    }

   #define DETAIL(...) do { if (printedDetails++ < 60) std::printf (__VA_ARGS__); } while (false)

    using ProcPtr = std::unique_ptr<PluginProcessor>;
    ProcPtr makeProc() { return std::make_unique<PluginProcessor>(); }

    float rawOf (PluginProcessor& p, const char* id)
    {
        auto* a = p.apvts.getRawParameterValue (id);
        return a != nullptr ? a->load() : -9999.0f;
    }

    void setReal (PluginProcessor& p, const char* id, float real)
    {
        if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id)))
            q->setValueNotifyingHost (q->convertTo0to1 (real));
    }

    void randomiseAll (PluginProcessor& p, juce::Random& rng)
    {
        for (auto* prm : p.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
                r->setValueNotifyingHost (rng.nextFloat());
    }

    bool allParamsSane (PluginProcessor& p)
    {
        for (const auto& m : warpr::kParamMeta)
        {
            const float v = rawOf (p, m.id);
            if (! std::isfinite (v) || v < m.minValue - 1.0e-3f || v > m.maxValue + 1.0e-3f)
                return false;
        }
        return true;
    }

    struct ParamCompare { int discrete = 0; int floatNotExact = 0; double worstRel = 0.0; };

    ParamCompare compareParams (PluginProcessor& a, PluginProcessor& b)
    {
        ParamCompare c;
        for (const auto& m : warpr::kParamMeta)
        {
            const float x = rawOf (a, m.id), y = rawOf (b, m.id);
            if (m.kind == warpr::ParamKind::Float)
            {
                if (! juce::exactlyEqual (x, y)) ++c.floatNotExact;
                c.worstRel = std::max (c.worstRel, (double) std::abs (x - y) / std::max (1.0, (double) std::abs (x)));
            }
            else if (! juce::exactlyEqual (x, y))
                ++c.discrete;
        }
        return c;
    }

    //==========================================================================
    // sources
    //==========================================================================
    // bits: 16 / 24 = integer-grid, 32 = arbitrary float32, 33 = float32 with special values
    juce::AudioBuffer<float> makeSource (int bits, int chans, int frames, juce::Random& rng)
    {
        juce::AudioBuffer<float> b (chans, frames);

        for (int c = 0; c < chans; ++c)
            for (int i = 0; i < frames; ++i)
            {
                float v = 0.0f;
                if (bits == 16)      v = (float) ((double) (rng.nextInt (65536) - 32768) / 32768.0);
                else if (bits == 24) v = (float) ((double) (rng.nextInt (16777216) - 8388608) / 8388608.0);
                else                 v = (rng.nextFloat() * 2.0f - 1.0f) * (0.1f + rng.nextFloat());
                b.setSample (c, i, v);
            }

        if (bits == 33 && frames > 8)
        {
            const float specials[] = { 1.0f, -0.0f, 1.0e-39f, 4.0f, -1.5f, -1.0f };
            for (int c = 0; c < chans; ++c)
                for (int i = 0; i < 6; ++i)
                    b.setSample (c, i, specials[i]);
        }
        return b;
    }

    warpr::SampleData::Ptr makeSample (const juce::AudioBuffer<float>& b, double sr, const juce::String& name)
    {
        return warpr::SampleData::create (b, sr, name, false, false);
    }

    bool sameSample (const warpr::SampleData& a, const warpr::SampleData& b)
    {
        if (a.numChannels != b.numChannels || a.numFrames != b.numFrames || ! juce::exactlyEqual (a.fileSampleRate, b.fileSampleRate)
            || a.name != b.name || a.singleCycle != b.singleCycle || a.isDefaultWave != b.isDefaultWave || a.numLevels != b.numLevels
            || a.peaksMinMax != b.peaksMinMax)
            return false;

        for (int k = 0; k < a.numLevels; ++k)
            if (a.level (k).numFrames != b.level (k).numFrames || a.level (k).storage != b.level (k).storage)
                return false;

        return true;
    }

    //==========================================================================
    // container parsing by hand (independent of StateIO)
    //==========================================================================
    struct Parsed
    {
        bool ok = false;
        juce::uint32 magic = 0, version = 0, xmlBytes = 0, blobBytes = 0;
        std::size_t countOffset = 0, blobStart = 0;
    };

    Parsed parseContainer (const juce::MemoryBlock& m)
    {
        Parsed p;
        const auto* b = static_cast<const juce::uint8*> (m.getData());
        if (m.getSize() < 16) return p;
        p.magic = juce::ByteOrder::littleEndianInt (b);
        p.version = juce::ByteOrder::littleEndianInt (b + 4);
        p.xmlBytes = juce::ByteOrder::littleEndianInt (b + 8);
        p.countOffset = 12 + (std::size_t) p.xmlBytes;
        if (p.countOffset + 4 > m.getSize()) return p;
        p.blobBytes = juce::ByteOrder::littleEndianInt (b + p.countOffset);
        p.blobStart = p.countOffset + 4;
        p.ok = true;
        return p;
    }

    juce::MemoryBlock repack (const juce::MemoryBlock& state, const std::function<juce::String (juce::String)>& xmlEdit,
                              juce::uint32 versionOverride, const juce::MemoryBlock& trailing)
    {
        const auto p = parseContainer (state);
        const auto* b = static_cast<const char*> (state.getData());
        auto xml = xmlEdit (juce::String::fromUTF8 (b + 12, (int) p.xmlBytes));
        juce::MemoryOutputStream out;
        out.writeInt ((int) p.magic);
        out.writeInt ((int) versionOverride);
        out.writeInt ((int) xml.getNumBytesAsUTF8());
        out.write (xml.toRawUTF8(), xml.getNumBytesAsUTF8());
        out.write (b + p.countOffset, (std::size_t) p.blobBytes + 4);
        out.write (trailing.getData(), trailing.getSize());
        return out.getMemoryBlock();
    }

    //==========================================================================
    // 1. identifiers + layout
    //==========================================================================
    void testLayout()
    {
        std::printf ("\n[D9] frozen identifiers + container layout\n");

        check (warpr::identity::containerMagic == 0x52505257u && std::string (warpr::identity::uiStateTag) == "WARPR_UI_STATE"
               && std::string (warpr::identity::apvtsStateId) == "Parameters" && warpr::identity::containerVersion == 1,
               "frozen wire-format identifiers unchanged (magic 'WRPR', WARPR_UI_STATE, Parameters, v1)");

        auto a = makeProc();
        const auto def = a->captureState();
        const auto pd = parseContainer (def);
        const auto* db = static_cast<const juce::uint8*> (def.getData());
        check (pd.ok && std::memcmp (db, "WRPR", 4) == 0 && pd.version == 1 && pd.blobBytes == 0
               && pd.blobStart == def.getSize(),
               "default state: bytes 'WRPR', version 1, XML, blob byte count 0, nothing after it (default wave is not embedded)");

        juce::Random rng (9001);
        auto buf = makeSource (24, 2, 1000, rng);
        const auto name = juce::String::fromUTF8 ("Sample \xC3\xA9 \xE2\x9C\x93");
        auto sd = makeSample (buf, 44100.25, name);
        a->getSampleStore().publish (sd);
        const auto st = a->captureState();
        const auto p = parseContainer (st);
        const auto* b = static_cast<const juce::uint8*> (st.getData());
        bool ok = p.ok && p.blobBytes > 0 && p.blobStart + p.blobBytes == st.getSize();
        if (ok)
        {
            const auto* h = b + p.blobStart;
            const auto nameBytes = (std::size_t) juce::ByteOrder::littleEndianShort (h + 19);
            double rate = 0.0;
            std::memcpy (&rate, h + 3, 8);
            const auto frames = juce::ByteOrder::littleEndianInt (h + 11);
            const auto payload = juce::ByteOrder::littleEndianInt (h + 21 + nameBytes);
            ok = h[0] == 1 && h[1] == 2 && h[2] == 0 && juce::exactlyEqual (rate, 44100.25) && frames == 1000u
                 && nameBytes == (std::size_t) name.getNumBytesAsUTF8() && std::memcmp (h + 21, name.toRawUTF8(), nameBytes) == 0
                 && p.blobBytes == 21 + nameBytes + 4 + payload;
        }
        check (ok, "sample blob: codec 1 (FLAC24), 2 channels, flags 0, f64 rate, u64 frames, u16+UTF-8 name, u32 payload, exact total size");
    }

    //==========================================================================
    // 2. codec
    //==========================================================================
    void testCodec()
    {
        std::printf ("\n[D9] sample codec\n");
        using namespace warpr::samplecodec;
        juce::Random rng (77);

        check (encode (*warpr::SampleData::createDefaultWave()).getSize() == 0, "default wave is never encoded");

        Status st = Status::ok;
        check (decode (nullptr, 0, st) == nullptr && st == Status::corrupt, "decode(nullptr) is corrupt");
        juce::uint8 shortBuf[10] = {};
        check (decode (shortBuf, sizeof (shortBuf), st) == nullptr && st == Status::corrupt, "decode(short buffer) is corrupt");

        // codec choice
        auto i24 = makeSample (makeSource (24, 2, 2000, rng), 48000.0, "i24");
        auto f32 = makeSample (makeSource (32, 2, 2000, rng), 48000.0, "f32");
        check (isLossless24 (*i24) && ! isLossless24 (*f32), "isLossless24: 24-bit grid yes, arbitrary float32 no");
        check (encode (*i24).getData() != nullptr && static_cast<const juce::uint8*> (encode (*i24).getData())[0] == 1, "24-bit grid source -> FLAC24");
        check (static_cast<const juce::uint8*> (encode (*f32).getData())[0] == 0, "float32 source -> raw float32");
        check (static_cast<const juce::uint8*> (encode (*i24, Choice::forceFloat32).getData())[0] == 0, "forceFloat32 honoured");

        // +1.0 and -0.0 are not 24-bit representable -> float32, still exact
        juce::AudioBuffer<float> edge (1, 100);
        edge.clear();
        edge.setSample (0, 3, 1.0f);
        edge.setSample (0, 4, -0.0f);
        auto e = makeSample (edge, 8000.0, "edge");
        auto eb = encode (*e);
        Status s2 = Status::corrupt;
        auto ed = decode (eb.getData(), eb.getSize(), s2);
        check (! isLossless24 (*e) && s2 == Status::ok && ed != nullptr && sameSample (*e, *ed), "+1.0 / -0.0 source: float32 fallback, bit-identical");

        // tiny samples round trip exactly whichever codec is used
        int tinyBad = 0;
        for (int frames : { 1, 2, 7, 16, 63, 64, 65 })
            for (int chans : { 1, 2 })
                for (int bits : { 16, 24, 32 })
                {
                    auto s = makeSample (makeSource (bits, chans, frames, rng), 22050.0, "tiny");
                    auto blob = encode (*s);
                    Status t = Status::corrupt;
                    auto d = decode (blob.getData(), blob.getSize(), t);
                    if (d == nullptr || ! sameSample (*s, *d)) { ++tinyBad; DETAIL ("    tiny %d frames %d ch %d bit failed\n", frames, chans, bits); }
                }
        check (tinyBad == 0, "1..65-frame mono/stereo samples round trip bit-identically");

        // over-long name is truncated to the u16 field and still decodes
        auto longName = makeSample (makeSource (24, 1, 100, rng), 48000.0, "x" + juce::String::repeatedString ("abcdefgh", 12000));
        auto ln = encode (*longName);
        Status s3 = Status::corrupt;
        auto lnd = decode (ln.getData(), ln.getSize(), s3);
        check (lnd != nullptr && lnd->name.getNumBytesAsUTF8() <= 65535 && lnd->name.startsWith ("xabcdefgh"), "96 KB name truncated to <= 65535 bytes, sample intact");

        // hostile lengths never allocate / crash
        auto good = encode (*i24);
        int hostileOk = 0, hostileN = 0;
        for (std::size_t pos = 0; pos < 40 && pos < good.getSize(); ++pos)
            for (int v : { 0, 1, 0x7f, 0x80, 0xff })
            {
                juce::MemoryBlock m (good);
                static_cast<juce::uint8*> (m.getData())[pos] = (juce::uint8) v;
                Status t = Status::ok;
                auto d = decode (m.getData(), m.getSize(), t);
                ++hostileN;
                if (d != nullptr || t == Status::corrupt || t == Status::defaultWave) ++hostileOk;
            }
        check (hostileOk == hostileN, "single-byte header mutations: always a clean verdict, no crash");
        std::printf ("        (%d header mutations)\n", hostileN);
    }

    //==========================================================================
    // 3. round trip matrix
    //==========================================================================
    void testRoundTripMatrix()
    {
        std::printf ("\n[D9] embedded sample round trip: 16/24-bit int, float32, mono/stereo, 6 file rates\n");
        auto a = makeProc();
        auto b = makeProc();
        juce::Random rng (4242);

        struct Src { const char* label; int bits, chans; juce::uint8 codec; };
        const Src sources[] = { { "i16 mono", 16, 1, 1 }, { "i16 stereo", 16, 2, 1 }, { "i24 mono", 24, 1, 1 }, { "i24 stereo", 24, 2, 1 },
                                { "f32 mono", 32, 1, 0 }, { "f32 stereo", 32, 2, 0 }, { "f32 special", 33, 2, 0 } };
        int bad = 0, wrongCodec = 0, total = 0;

        for (double fileRate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 44100.25 })
        {
            const double host = fileRate > 100000.0 ? 96000.0 : 48000.0;
            for (auto* p : { a.get(), b.get() }) { p->setPlayConfigDetails (0, 2, host, 256); p->prepareToPlay (host, 256); }

            for (const auto& s : sources)
            {
                auto sd = makeSample (makeSource (s.bits, s.chans, 2500, rng), fileRate, juce::String (s.label));
                a->getSampleStore().publish (sd);
                const auto state = a->captureState();
                const auto pc = parseContainer (state);
                if (static_cast<const juce::uint8*> (state.getData())[pc.blobStart] != s.codec) ++wrongCodec;

                const auto outcome = b->loadStateBlock (state.getData(), state.getSize());
                auto got = b->getSampleStore().latest();
                ++total;
                if (outcome != warpr::state::LoadOutcome::loaded || got == nullptr || ! sameSample (*sd, *got))
                {
                    ++bad;
                    DETAIL ("    FAIL %s @ %.2f Hz\n", s.label, fileRate);
                }

                juce::AudioBuffer<float> blk (2, 256);
                juce::MidiBuffer m;
                m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
                b->processBlock (blk, m);                      // adoption on the audio side must be safe and finite
                if (! warpr::tests::scan (blk, 0).allFinite) ++bad;
            }
        }
        check (bad == 0, "every combination: SampleData (level 0, all mips, peaks, name, rate) bit-identical after the round trip");
        check (wrongCodec == 0, "codec per source kind: integer grid -> FLAC24, float -> float32");
        std::printf ("        (%d round trips)\n", total);
    }

    //==========================================================================
    // 4. params + UI state + default wave
    //==========================================================================
    void testParamsAndUiState()
    {
        std::printf ("\n[D9] 135 parameters + UI state + default wave\n");
        auto a = makeProc();
        juce::Random rng (20261113);
        randomiseAll (*a, rng);
        a->getBackendImpl().servicePending();

        auto& be = a->getBackendImpl();
        be.setLfoCount (8);
        for (int i = 0; i < 8; ++i)
            be.setLfoShapeJson (i, juce::String::formatted (R"({"preset":"CUSTOM","points":[{"x":0,"v":0.%d,"c":0},{"x":0.5,"v":0.9,"c":0.3},{"x":1,"v":0.2,"c":-0.4}],"bends":[],"table":null,"drift":null})", i + 1));
        be.setModMatrixJson (R"([{"src":2,"dst":"filterCutoff","depth":0.5},{"src":5,"dst":"warpAmount","depth":0.25}])");
        be.setShaperCurveJson ("A", R"({"interp":"smooth","points":[{"x":-1,"y":-1},{"x":0,"y":0},{"x":0.4,"y":0.9},{"x":1,"y":1}]})");
        be.setShaperCurveJson ("B", R"({"interp":"linear","points":[{"x":-1,"y":-1},{"x":0.5,"y":0.8},{"x":1,"y":1}]})");
        be.servicePending();
        be.setPresetName ("Round Trip");

        juce::Random srng (5);
        auto sd = makeSample (makeSource (24, 2, 6000, srng), 44100.0, "Stereo24");
        a->getSampleStore().publish (sd);

        const auto s1 = a->captureState();
        auto b = makeProc();
        b->getSampleStore().publish (makeSample (makeSource (32, 1, 500, srng), 8000.0, "stale"));
        b->loadStateBlock (s1.getData(), s1.getSize());

        const auto c1 = compareParams (*a, *b);
        std::printf ("        (A->B: %d floats not bit-exact, worst relative deviation %.3g)\n", c1.floatNotExact, c1.worstRel);
        check (c1.discrete == 0, "Int/Bool/Choice parameters bit-exact");
        check (c1.worstRel <= 1.0e-6, "Float parameters within 1e-6 relative (APVTS normalised round trip; see report)");
        check (a->getUiBackend().getUiStateJson() == b->getUiBackend().getUiStateJson(), "UI state JSON (LFO shapes, matrix, curves, count) identical");
        check (b->getUiBackend().getPresetName() == "Round Trip", "preset name persisted in the state");
        auto got = b->getSampleStore().latest();
        check (got != nullptr && sameSample (*sd, *got), "embedded sample replaced the stale one, bit-identical");

        const auto s2 = b->captureState();
        auto c = makeProc();
        c->loadStateBlock (s2.getData(), s2.getSize());
        const auto c2 = compareParams (*b, *c);
        std::printf ("        (B->C: %d floats not bit-exact; blob(B) %s blob(C))\n", c2.floatNotExact, s2 == c->captureState() ? "==" : "!=");
        check (c2.discrete == 0 && c2.worstRel <= 1.0e-6, "second generation stays within tolerance (no drift)");

        // default wave: not embedded, regenerated
        auto fresh = makeProc();
        const auto sdef = fresh->captureState();
        b->loadStateBlock (sdef.getData(), sdef.getSize());
        auto after = b->getSampleStore().latest();
        auto reference = warpr::SampleData::createDefaultWave();
        check (after != nullptr && after->isDefaultWave && sameSample (*after, *reference), "state without a blob -> default wave regenerated, identical to a fresh one");
        check (compareParams (*fresh, *b).discrete == 0 && b->getUiBackend().getPresetName() == "Init", "defaults state restores defaults");

        // revision audit for a state load
        auto r = [&b] (WarprUiBackend::Revision rev) { return b->getUiBackend().revision (rev); };
        const juce::uint32 before[5] = { r (WarprUiBackend::Revision::uiState), r (WarprUiBackend::Revision::sample), r (WarprUiBackend::Revision::shaperCurves),
                                     r (WarprUiBackend::Revision::filterResponse), r (WarprUiBackend::Revision::preset) };
        b->loadStateBlock (s1.getData(), s1.getSize());
        check (r (WarprUiBackend::Revision::uiState) > before[0] && r (WarprUiBackend::Revision::sample) > before[1]
               && r (WarprUiBackend::Revision::shaperCurves) > before[2] && r (WarprUiBackend::Revision::filterResponse) > before[3]
               && r (WarprUiBackend::Revision::preset) > before[4], "state load bumps all five revisions");
    }

    //==========================================================================
    // 5. audio equality
    //==========================================================================
    void curatePatch (PluginProcessor& p)
    {
        setReal (p, "warpEnable", 1.0f); setReal (p, "warpMode", 22.0f); setReal (p, "warpAmount", 0.55f);
        setReal (p, "filterEnable", 1.0f); setReal (p, "filterType", 3.0f); setReal (p, "filterCutoff", 2500.0f); setReal (p, "filterResonance", 0.3f);
        setReal (p, "unisonVoices", 3.0f); setReal (p, "ampRelease", 200.0f);
        p.getBackendImpl().setLfoCount (3);
        p.getBackendImpl().setModMatrixJson (R"([{"src":1,"dst":"filterCutoff","depth":0.4}])");
    }

    juce::AudioBuffer<float> renderBlocks (PluginProcessor& p, const juce::MidiBuffer& midi, double fs, int blockSize, double seconds,
                                           const std::function<void (int)>& hook)
    {
        p.setPlayConfigDetails (0, 2, fs, blockSize);
        p.prepareToPlay (fs, blockSize);
        const int total = (int) std::ceil (fs * seconds);
        juce::AudioBuffer<float> out (2, total);
        out.clear();
        juce::AudioBuffer<float> scratch (2, blockSize);
        juce::MidiBuffer blockMidi;
        auto it = midi.begin();
        int blockIndex = 0;

        for (int pos = 0; pos < total; pos += blockSize, ++blockIndex)
        {
            const int n = std::min (blockSize, total - pos);
            blockMidi.clear();
            while (it != midi.end() && (*it).samplePosition < pos + n)
            {
                blockMidi.addEvent ((*it).getMessage(), (*it).samplePosition - pos);
                ++it;
            }
            juce::AudioBuffer<float> block (scratch.getArrayOfWritePointers(), 2, n);
            block.clear();
            if (hook) hook (blockIndex);
            p.processBlock (block, blockMidi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, block, ch, 0, n);
        }
        p.releaseResources();
        return out;
    }

    bool identical (const juce::AudioBuffer<float>& x, const juce::AudioBuffer<float>& y)
    {
        if (x.getNumSamples() != y.getNumSamples()) return false;
        for (int c = 0; c < 2; ++c)
            if (std::memcmp (x.getReadPointer (c), y.getReadPointer (c), (std::size_t) x.getNumSamples() * sizeof (float)) != 0)
                return false;
        return true;
    }

    void testAudioEquality()
    {
        std::printf ("\n[D9] state-loaded processors render identically (4 host sample rates)\n");
        auto a = makeProc();
        curatePatch (*a);
        juce::Random rng (31);
        a->getSampleStore().publish (makeSample (makeSource (24, 2, 20000, rng), 48000.0, "Tone"));
        const auto state = a->captureState();

        auto b = makeProc(), c = makeProc();
        b->loadStateBlock (state.getData(), state.getSize());
        c->loadStateBlock (state.getData(), state.getSize());

        int mismatch = 0, silent = 0;
        double worstVsOriginal = 0.0;
        for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const auto midi = warpr::tests::singleNote (57, 0.8f, fs, 0.2);
            const auto rb = renderBlocks (*b, midi, fs, 256, 0.35, nullptr);
            const auto rc = renderBlocks (*c, midi, fs, 256, 0.35, nullptr);
            const auto ra = renderBlocks (*a, midi, fs, 256, 0.35, nullptr);
            if (! identical (rb, rc)) ++mismatch;
            if (warpr::tests::scan (rb, 0).peak < 0.01f) ++silent;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < rb.getNumSamples(); ++i)
                    worstVsOriginal = std::max (worstVsOriginal, (double) std::abs (rb.getSample (ch, i) - ra.getSample (ch, i)));
        }
        check (mismatch == 0, "two processors loaded from one state render bit-identically at 44.1/48/96/192 kHz");
        check (silent == 0, "the restored patch is audible");
        // hand-derived: the original differs from the loaded copy only by float parameter rounding (~1e-7 relative) in a non-chaotic patch
        std::printf ("        (max |loaded - original| = %.3g)\n", worstVsOriginal);
        check (worstVsOriginal < 1.0e-3, "loaded copy matches the original within 1e-3 (parameter rounding only)");

        // no audio-thread allocation after a restore that swapped the sample
        b->loadStateBlock (state.getData(), state.getSize());
        b->setPlayConfigDetails (0, 2, 48000.0, 256);
        b->prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> blk (2, 256);
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
        b->processBlock (blk, m);
        std::size_t allocs = 1;
        {
            warpr::tests::ScopedAllocCount counter;
            for (int i = 0; i < 50; ++i) b->processBlock (blk, m);
            allocs = counter.count();
        }
        check (allocs == 0, "processBlock allocates nothing after a state restore");
    }

    //==========================================================================
    // 6. forward compatibility / migration
    //==========================================================================
    void testCompatibility()
    {
        std::printf ("\n[D9] forward compatibility, migration hooks, legacy payload\n");
        auto a = makeProc();
        juce::Random rng (6);
        setReal (*a, "warpAmount", 0.77f);
        a->getBackendImpl().setLfoCount (6);
        a->getSampleStore().publish (makeSample (makeSource (24, 1, 3000, rng), 48000.0, "C"));
        const auto good = a->captureState();
        const auto p = parseContainer (good);

        auto probe = makeProc();
        auto loads = [&probe] (const juce::MemoryBlock& m) { return probe->loadStateBlock (m.getData(), m.getSize()); };
        using O = warpr::state::LoadOutcome;

        // legacy = a pre-D9 container: identical layout, blob byte count 0 (the old writer always wrote the 0)
        const auto legacy = repack (good, [] (juce::String s) { return s; }, 1, {});
        juce::MemoryBlock legacyNoBlob;
        {
            juce::MemoryOutputStream out;
            out.write (good.getData(), 12 + (std::size_t) p.xmlBytes);
            out.writeInt (0);
            legacyNoBlob = out.getMemoryBlock();
        }
        auto lp = makeProc();
        lp->getSampleStore().publish (makeSample (makeSource (24, 1, 500, rng), 48000.0, "keep?"));
        check (lp->loadStateBlock (legacyNoBlob.getData(), legacyNoBlob.getSize()) == O::loaded
               && std::abs (rawOf (*lp, "warpAmount") - 0.77f) < 1.0e-6f && lp->getBackendImpl().copyModel().lfoCount == 6
               && lp->getSampleStore().latest()->isDefaultWave,
               "legacy payload (blob byte count 0) loads: parameters + UI state, default wave");
        check (loads (legacy) == O::loaded, "repacked identical container loads");

        check (loads (repack (good, [] (juce::String s) { return s; }, 2, juce::MemoryBlock ("trailing-future-data", 20))) == O::loaded,
               "newer container version + trailing bytes: read best-effort");
        check (loads (repack (good, [] (juce::String s) { return s; }, 0, {})) == O::rejected, "container version 0 rejected");

        const juce::String tag = juce::String ("warprStateVersion=\"") + juce::String (warpr::identity::uiStateVersion) + "\"";
        check (loads (repack (good, [&tag] (juce::String s) { return s.replace (tag, "warprStateVersion=\"99\""); }, 1, {})) == O::loaded
               && probe->getBackendImpl().copyModel().lfoCount == 6, "newer warprStateVersion: known fields still load");
        check (loads (repack (good, [&tag] (juce::String s) { return s.replace (tag, ""); }, 1, {})) == O::loaded, "missing warprStateVersion loads (treated as v1)");

        auto badCodec = good;
        static_cast<juce::uint8*> (badCodec.getData())[p.blobStart] = 7;
        auto q = makeProc();
        q->getSampleStore().publish (makeSample (makeSource (32, 1, 300, rng), 8000.0, "will be replaced"));
        check (q->loadStateBlock (badCodec.getData(), badCodec.getSize()) == O::loadedDefaultSampleAfterCorruptBlob
               && std::abs (rawOf (*q, "warpAmount") - 0.77f) < 1.0e-6f && q->getSampleStore().latest()->isDefaultWave,
               "unknown codec: parameters load, default wave used, reported as corrupt blob");

        auto victim = makeProc();
        const auto pristine = victim->captureState();
        juce::MemoryBlock cut (good.getData(), good.getSize() - 10);
        check (victim->loadStateBlock (cut.getData(), cut.getSize()) == O::rejected && victim->captureState() == pristine, "truncated blob: rejected, state untouched");

        std::string bomb = "<Parameters>";
        for (int i = 0; i < 100000; ++i) bomb += "<a>";
        juce::MemoryOutputStream out;
        out.writeInt ((int) warpr::identity::containerMagic); out.writeInt (1); out.writeInt ((int) bomb.size());
        out.write (bomb.data(), bomb.size()); out.writeInt (0);
        const auto bm = out.getMemoryBlock();
        check (victim->loadStateBlock (bm.getData(), bm.getSize()) == O::rejected && victim->captureState() == pristine, "100000-deep XML rejected without recursion");

        const std::string ok1 = "<a x=\"/>\"><b/></a>", ok2 = "<a><b><c/></b></a><!-- <x><x><x> -->";
        std::string deep;
        for (int i = 0; i < 300; ++i) deep += "<a>";
        check (! warpr::state::xmlNestsTooDeeply (ok1.data(), ok1.size(), 2) && ! warpr::state::xmlNestsTooDeeply (ok2.data(), ok2.size(), 3)
               && warpr::state::xmlNestsTooDeeply (deep.data(), deep.size()), "xmlNestsTooDeeply: quotes / comments / self-closing handled, 300 levels flagged");
    }

    //==========================================================================
    // 7. fuzz
    //==========================================================================
    juce::MemoryBlock mutate (const juce::MemoryBlock& base, int kind, juce::Random& rng, std::size_t countOffset)
    {
        juce::MemoryBlock m (base);
        const auto size = m.getSize();
        auto* b = static_cast<juce::uint8*> (m.getData());
        const int isz = (int) size;

        switch (kind % 8)
        {
            case 0: for (int f = 0, n = 1 + rng.nextInt (8); f < n; ++f) b[rng.nextInt (isz)] = (juce::uint8) rng.nextInt (256); break;
            case 1:
                for (int f = 0, n = 1 + rng.nextInt (4); f < n; ++f)
                {
                    const std::size_t pos = rng.nextBool() ? (std::size_t) rng.nextInt (std::min (64, isz))
                                                           : std::min (size - 1, countOffset + (std::size_t) rng.nextInt (48));
                    b[pos] = (juce::uint8) rng.nextInt (256);
                }
                break;
            case 2: m.setSize ((std::size_t) rng.nextInt (isz)); break;
            case 3: m.removeSection ((std::size_t) rng.nextInt (isz - 1), (std::size_t) (1 + rng.nextInt (64))); break;
            case 4:
            {
                juce::MemoryBlock extra ((std::size_t) (1 + rng.nextInt (64)));
                for (std::size_t i = 0; i < extra.getSize(); ++i) static_cast<juce::uint8*> (extra.getData())[i] = (juce::uint8) rng.nextInt (256);
                m.insert (extra.getData(), extra.getSize(), (std::size_t) rng.nextInt (isz));
                break;
            }
            case 5:
                m.setSize ((std::size_t) (1 + rng.nextInt (512)));
                for (std::size_t i = 0; i < m.getSize(); ++i) static_cast<juce::uint8*> (m.getData())[i] = (juce::uint8) rng.nextInt (256);
                break;
            case 6:
            {
                const std::size_t offsets[] = { 4, 8, countOffset };
                const juce::uint32 values[] = { 0xffffffffu, 0u, 0x7fffffffu, 1u, (juce::uint32) size, (juce::uint32) size + 1u, 0x80000000u };
                const auto off = offsets[rng.nextInt (3)];
                const auto v = values[rng.nextInt (7)];
                for (int k = 0; k < 4; ++k) b[off + (std::size_t) k] = (juce::uint8) ((v >> (8 * k)) & 0xffu);
                break;
            }
            default:
            {
                const auto start = (std::size_t) rng.nextInt (isz);
                const auto len = std::min (size - start, (std::size_t) (1 + rng.nextInt (200)));
                std::memset (b + start, 0, len);
                if (rng.nextBool()) m.append (base.getData(), base.getSize());
                break;
            }
        }
        return m;
    }

    void testFuzz (bool full)
    {
        std::printf ("\n[D9] state fuzz (seeded)\n");
        using O = warpr::state::LoadOutcome;
        auto src = makeProc();
        juce::Random srng (123);
        src->getSampleStore().publish (makeSample (makeSource (24, 2, 4000, srng), 48000.0, "FuzzFlac"));
        const auto flacState = src->captureState();
        src->getSampleStore().publish (makeSample (makeSource (32, 2, 3000, srng), 44100.0, "FuzzFloat"));
        const auto floatState = src->captureState();

        auto victim = makeProc();
        const auto pristine = victim->captureState();

        // every strict prefix is a truncation: all rejected, nothing touched; the full length is accepted
        int prefixBad = 0, prefixes = 0;
        for (std::size_t len = 0; len < flacState.getSize(); len += (len < 200 ? 1 : 7))
        {
            ++prefixes;
            if (victim->loadStateBlock (flacState.getData(), len) != O::rejected) ++prefixBad;
        }
        check (prefixBad == 0 && victim->captureState() == pristine, "all strict prefixes rejected and the state is untouched (defaults retained)");
        std::printf ("        (%d prefixes)\n", prefixes);
        check (victim->loadStateBlock (flacState.getData(), flacState.getSize()) == O::loaded, "the full container is accepted");

        const int count = full ? 6000 : 1200;
        juce::Random rng (0xD9F022);
        int rejected = 0, loaded = 0, fallback = 0, badReject = 0, badAccept = 0;

        for (int i = 0; i < count; ++i)
        {
            const auto& base = (i & 1) ? floatState : flacState;
            const auto countOffset = parseContainer (base).countOffset;
            const auto m = mutate (base, i / 2, rng, countOffset);
            const auto before = victim->captureState();
            const auto outcome = victim->loadStateBlock (m.getData(), m.getSize());

            if (outcome == O::rejected)
            {
                ++rejected;
                if (victim->captureState() != before) { ++badReject; DETAIL ("    mutation %d changed the state although rejected\n", i); }
            }
            else
            {
                if (outcome == O::loaded) ++loaded; else ++fallback;
                auto s = victim->getSampleStore().latest();
                bool sane = allParamsSane (*victim) && s != nullptr && s->numChannels >= 1 && s->numChannels <= 2 && s->numFrames > 0;
                if (sane) sane = warpr::tests::scan (s->level (0).channel (0), std::min (s->numFrames, 4000)).allFinite;
                const int lc = victim->getBackendImpl().copyModel().lfoCount;
                if (! sane || lc < 1 || lc > 10) { ++badAccept; DETAIL ("    mutation %d accepted but state insane\n", i); }
            }

            if (i % 100 == 99)
                victim->loadStateBlock (flacState.getData(), flacState.getSize());
        }

        std::printf ("        (%d mutations: %d rejected, %d loaded, %d sample-fallback; no crash)\n", count, rejected, loaded, fallback);
        check (badReject == 0, "every rejected mutation left parameters, UI state and sample untouched");
        check (badAccept == 0, "every accepted mutation left sane parameters, a valid sample and lfoCount in 1..10");
        check (count >= 1000, ">= 1000 seeded mutations executed");
    }

    //==========================================================================
    // 8. presets
    //==========================================================================
    struct TempDir
    {
        juce::File dir;
        explicit TempDir (const juce::String& tag)
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("VOID_D9_" + tag + "_" + juce::String (juce::Time::currentTimeMillis())))
        { dir.createDirectory(); }
        ~TempDir() { dir.deleteRecursively(); }
    };

    void testPresets()
    {
        std::printf ("\n[D9] presets: 20 generated, step / wrap / skip corrupt, presetName revision\n");
        TempDir tmp ("presets");
        const auto presetDir = tmp.dir.getChildFile ("Presets");
        auto a = makeProc();
        a->getPresetManager().setDirectory (presetDir);
        std::vector<juce::String> errors;
        a->getBackendImpl().setErrorReporter ([&errors] (const juce::String& e) { errors.push_back (e); });
        auto& be = a->getBackendImpl();

        juce::Random rng (808);
        std::vector<float> amounts;
        for (int i = 0; i < 20; ++i)
        {
            const float amt = (float) i / 19.0f;
            amounts.push_back (amt);
            setReal (*a, "warpAmount", amt);
            be.setLfoCount (1 + i % 10);
            if (i % 5 == 0)
                a->getSampleStore().publish (makeSample (makeSource (24, 1 + i % 2, 1500, rng), 48000.0, "S" + juce::String (i)));
            else
                a->getSampleStore().publish (warpr::SampleData::createDefaultWave());
            const auto rev = be.revision (WarprUiBackend::Revision::preset);
            const auto ui = be.revision (WarprUiBackend::Revision::uiState);
            check (be.saveUserPreset ("Preset " + juce::String (i).paddedLeft ('0', 2)) && be.revision (WarprUiBackend::Revision::preset) == rev + 1
                   && be.revision (WarprUiBackend::Revision::uiState) == ui, "save bumps only Revision::preset");
        }

        const auto entries = a->getPresetManager().listEntries();
        bool sorted = entries.size() == 20, ext = true;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            sorted = sorted && entries[i].name == "Preset " + juce::String ((int) i).paddedLeft ('0', 2);
            ext = ext && entries[i].file.hasFileExtension (warpr::identity::presetExtension);
        }
        check (sorted && ext && errors.empty(), "20 .void files listed in name order, no errors");

        {
            juce::MemoryBlock fileBytes;
            juce::String err;
            warpr::PresetManager::readFile (entries[0].file, fileBytes, err);
            check (fileBytes.getSize() > 16 && std::memcmp (fileBytes.getData(), "WRPR", 4) == 0, "preset file starts with the container magic");
        }

        auto b = makeProc();
        b->getPresetManager().setDirectory (presetDir);
        b->getBackendImpl().setErrorReporter ([&errors] (const juce::String& e) { errors.push_back (e); });
        auto& bb = b->getBackendImpl();
        int wrongName = 0, wrongParam = 0, wrongLfo = 0, wrongSample = 0, noEvent = 0;

        for (int i = 0; i < 20; ++i)
        {
            const auto rev = bb.revision (WarprUiBackend::Revision::preset);
            bb.presetStep (+1);
            if (bb.getPresetName() != "Preset " + juce::String (i).paddedLeft ('0', 2)) ++wrongName;
            if (std::abs (rawOf (*b, "warpAmount") - amounts[(size_t) i]) > 1.0e-5f) ++wrongParam;
            if (bb.copyModel().lfoCount != 1 + i % 10) ++wrongLfo;
            auto s = b->getSampleStore().latest();
            const bool wantSample = i % 5 == 0;
            if (s == nullptr || (wantSample ? (s->isDefaultWave || s->name != "S" + juce::String (i)) : ! s->isDefaultWave)) ++wrongSample;
            if (bb.revision (WarprUiBackend::Revision::preset) <= rev) ++noEvent;
        }
        check (wrongName == 0, "20 steps from 'Init' visit Preset 00..19 in order (presetName follows the file)");
        check (wrongParam == 0 && wrongLfo == 0, "each loaded preset restores its parameters and UI state");
        check (wrongSample == 0, "embedded samples restored, and replaced by the default wave when a preset has none");
        check (noEvent == 0, "every step bumps Revision::preset (presetName event)");
        bb.presetStep (+1);
        check (bb.getPresetName() == "Preset 00", "step +1 wraps from the last to the first");
        bb.presetStep (-1);
        check (bb.getPresetName() == "Preset 19", "step -1 wraps from the first to the last");

        // corrupt file between two good ones
        const auto bad = presetDir.getChildFile ("Preset 05b broken" + juce::String (warpr::identity::presetExtension));
        bad.replaceWithText ("this is not a preset");
        bb.loadPresetFile (presetDir.getChildFile ("Preset 05" + juce::String (warpr::identity::presetExtension)));
        errors.clear();
        bb.presetStep (+1);
        check (bb.getPresetName() == "Preset 06" && errors.empty(), "step skips an unreadable file silently and lands on the next good preset");
        const float amt6 = rawOf (*b, "warpAmount");
        const auto stateBefore = b->captureState();
        check (! bb.loadPresetFile (bad) && ! errors.empty() && bb.getPresetName() == "Preset 06" && rawOf (*b, "warpAmount") == amt6
               && b->captureState() == stateBefore, "loading a corrupt file reports an error and leaves the state untouched");

        // sanitising, extension handling, failed save
        check (warpr::PresetManager::sanitiseName ("a/b:c") .containsAnyOf ("/:") == false && warpr::PresetManager::sanitiseName ("") == "Untitled"
               && warpr::PresetManager::sanitiseName ("..hidden") == "hidden", "sanitiseName: separators removed, never empty, no leading dots");
        errors.clear();
        const auto okFile = tmp.dir.getChildFile ("x.preset");
        check (bb.savePresetToFile (okFile) && tmp.dir.getChildFile ("x.preset" + juce::String (warpr::identity::presetExtension)).existsAsFile()
               && bb.getPresetName() == "x.preset", "save appends the extension when it is missing");
        const auto prevName = bb.getPresetName();
        const auto blocker = tmp.dir.getChildFile ("blocker.txt");      // a FILE as the parent folder: the write cannot succeed
        blocker.replaceWithText ("x");
        check (! bb.savePresetToFile (blocker.getChildFile ("y")) && bb.getPresetName() == prevName && ! errors.empty(),
               "failed save reports an error and keeps the name");

        // state carries the name; factory infrastructure
        const auto st = b->captureState();
        auto c = makeProc();
        c->loadStateBlock (st.getData(), st.getSize());
        check (c->getBackendImpl().getPresetName() == prevName, "preset name travels with the state");

        std::vector<warpr::FactoryPreset> factory;
        factory.push_back ({ "Factory One", st });
        factory.push_back ({ "Factory Two", a->captureState() });
        warpr::PresetManager withFactory (presetDir, factory);
        const auto fe = withFactory.listEntries();
        check (fe.size() == 23 && fe[0].factory && fe[1].factory && ! fe[2].factory
               && fe[0].name == "Factory One", "factory presets are listed first, then user files");
        auto d = makeProc();
        d->getPresetManager().setDirectory (presetDir);
        d->getBackendImpl().attachPresetManager (&withFactory);
        d->getBackendImpl().presetStep (+1);
        check (d->getBackendImpl().getPresetName() == "Factory One", "stepping from 'Init' reaches the first factory preset");
        check (warpr::PresetManager::builtInFactoryPresets().empty(), "built-in factory list is empty in D9");
    }

    //==========================================================================
    // 9. revisions + automation list
    //==========================================================================
    void testRevisionsAndAutomation()
    {
        std::printf ("\n[D9] revision audit + automation list\n");
        static_assert ((int) WarprUiBackend::Revision::preset == 4, "revision enum layout");

        auto p = makeProc();
        auto& params = p->getParameters();
        std::set<juce::String> ids;
        int automatable = 0, hidden = 0, hiddenWrong = 0, emptyNames = 0, badText = 0;
        for (auto* prm : params)
        {
            auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm);
            if (r == nullptr) continue;
            ids.insert (r->paramID);
            if (r->getName (64).isEmpty()) ++emptyNames;
            if (r->getText (r->getValue(), 32).isEmpty()) ++badText;
            if (r->isAutomatable()) ++automatable;
            else
            {
                ++hidden;
                const auto id = r->paramID;
                if (! (id.startsWith ("lfo") && (id.endsWith ("GridX") || id.endsWith ("GridY")))) ++hiddenWrong;
            }
        }
        check (params.size() == 135 && ids.size() == 135, "135 unique parameters");
        check (automatable == 115 && hidden == 20 && hiddenWrong == 0, "115 automatable + 20 hidden (lfoNGridX/Y only)");
        check (emptyNames == 0 && badText == 0, "every parameter has a name and a default display text");

        auto& be = p->getBackendImpl();
        const auto rs = be.revision (WarprUiBackend::Revision::sample);
        be.setPresetName ("X");
        check (be.revision (WarprUiBackend::Revision::sample) == rs && be.getPresetName() == "X", "setPresetName bumps only the preset revision");
    }

    //==========================================================================
    // 10. app settings
    //==========================================================================
    void testSettings()
    {
        std::printf ("\n[D9] app settings\n");
        TempDir tmp ("settings");
        warpr::AppSettings s (tmp.dir.getChildFile ("sub").getChildFile ("Settings.xml"));
        check (s.load() == warpr::AppSettingsData{}, "missing file -> defaults");
        warpr::AppSettingsData d;
        d.oscillatorBudget = 512; d.interpolationQuality = 1; d.cpuGuard = true;
        check (s.save (d) && s.load() == d, "save / load round trip (parent folder created)");
        s.getFile().replaceWithText ("<Settings oscillatorBudget=\"5000\" interpolationQuality=\"9\" cpuGuard=\"1\"/>");
        const auto c = s.load();
        check (c.oscillatorBudget == 1024 && c.interpolationQuality == 1 && c.cpuGuard, "out-of-range values clamped");
        s.getFile().replaceWithText ("<Settings oscillatorBudget=\"10\"/>");
        check (s.load().oscillatorBudget == 64, "small budgets clamp to 64");
        s.getFile().replaceWithText ("<Settings oscillatorBudget=\"-5\"/>");
        check (s.load().oscillatorBudget == 0, "non-positive budget means off");
        s.getFile().replaceWithText ("garbage <<<");
        check (s.load() == warpr::AppSettingsData{}, "unreadable file -> defaults");

        auto p = makeProc();
        check (p->getSynth().getOscillatorBudget() == 256, "direct construction ignores the user's settings file (default budget 256)");
        warpr::AppSettingsData e;
        e.oscillatorBudget = 128; e.cpuGuard = true;
        p->applyAppSettings (e);
        check (p->getSynth().getOscillatorBudget() == 128 && p->isCpuGuardRequested(), "applyAppSettings reaches the synth and the guard flag");
    }

    //==========================================================================
    // 11. host-state size record
    //==========================================================================
    void testStateSizes (bool full)
    {
        std::printf ("\n[D9] host-state size record\n");
        auto p = makeProc();
        std::printf ("        default state (no sample): %zu bytes\n", p->captureState().getSize());

        const double seconds = full ? 30.0 : 5.0;
        const int frames = (int) (48000.0 * seconds);
        juce::AudioBuffer<float> tone (2, frames);
        for (int i = 0; i < frames; ++i)
        {
            const double t = (double) i / 48000.0;
            const double v = 0.5 * std::sin (2.0 * 3.14159265358979 * 220.0 * t) + 0.25 * std::sin (2.0 * 3.14159265358979 * 1337.0 * t);
            tone.setSample (0, i, (float) (std::floor (v * 8388608.0) / 8388608.0));
            tone.setSample (1, i, (float) (std::floor (0.8 * v * 8388608.0) / 8388608.0));
        }
        auto sd = makeSample (tone, 48000.0, "Tone");
        p->getSampleStore().publish (sd);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto flacState = p->captureState();
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        const auto again = p->captureState();
        const auto t2 = juce::Time::getMillisecondCounterHiRes();
        const auto rawSize = warpr::samplecodec::encode (*sd, warpr::samplecodec::Choice::forceFloat32).getSize();
        std::printf ("        %.0f s stereo 48 kHz 24-bit tone: FLAC24 state %zu bytes (encode+verify %.0f ms), float32 blob %zu bytes, ratio %.2f, cached capture %.1f ms\n",
                     seconds, flacState.getSize(), t1 - t0, rawSize, (double) flacState.getSize() / (double) rawSize, t2 - t1);
        // hand-derived: a quantised two-sine signal is very predictable, FLAC must beat 85 % of raw float32 comfortably
        check ((double) flacState.getSize() < 0.85 * (double) rawSize, "FLAC24 smaller than 85 % of the float32 blob for tonal 24-bit material");
        check (again == flacState && p->getSampleBlobCache().getNumEncodes() == 1, "repeated capture is cached (one encode) and deterministic");
        p->getSampleStore().publish (warpr::SampleData::createDefaultWave());
        p->captureState();
        check (p->getSampleBlobCache().getNumEncodes() == 1, "the default wave is never encoded");
        p->getSampleStore().publish (makeSample (tone, 48000.0, "Tone2"));      // a new SampleData (new serial)
        p->captureState();
        check (p->getSampleBlobCache().getNumEncodes() == 2, "a replaced sample is re-encoded");
    }

    //==========================================================================
    // 12. soak-style determinism
    //==========================================================================
    juce::MidiBuffer makeStorm (double fs, double seconds, juce::Random& rng)
    {
        juce::MidiBuffer m;
        const int end = (int) (fs * seconds);
        int t = 0;
        while (t < end)
        {
            const int note = 36 + rng.nextInt (60);
            const int len = (int) (fs * (0.05 + 0.6 * rng.nextDouble()));
            m.addEvent (juce::MidiMessage::noteOn (1, note, 0.3f + 0.7f * rng.nextFloat()), t);
            m.addEvent (juce::MidiMessage::noteOff (1, note), std::min (end - 1, t + len));
            t += (int) (fs * (0.03 + 0.2 * rng.nextDouble()));
        }
        m.addEvent (juce::MidiMessage::pitchWheel (1, 12000), end / 3);
        m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), end / 2);
        m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), end / 2 + (int) (fs * 0.5));
        return m;
    }

    void testSoak (bool full)
    {
        std::printf ("\n[D9] soak-style determinism (MIDI storm)\n");
        const double fs = 48000.0, seconds = full ? 30.0 : 6.0;
        juce::Random mrng (99);
        const auto midi = makeStorm (fs, seconds, mrng);

        auto src = makeProc();
        curatePatch (*src);
        juce::Random srng (3);
        src->getSampleStore().publish (makeSample (makeSource (24, 2, 30000, srng), 48000.0, "Soak"));
        const auto state = src->captureState();

        auto p1 = makeProc(), p2 = makeProc(), p3 = makeProc();
        for (auto* p : { p1.get(), p2.get(), p3.get() }) p->loadStateBlock (state.getData(), state.getSize());

        int captures = 0;
        const auto r1 = renderBlocks (*p1, midi, fs, 256, seconds, nullptr);
        const auto r2 = renderBlocks (*p2, midi, fs, 256, seconds, [&] (int blockIndex)
        {
            if (blockIndex % 40 == 0) { p2->captureState(); p2->getUiBackend().getUiStateJson(); ++captures; }
        });
        const auto r3 = renderBlocks (*p3, midi, fs, 256, seconds, nullptr);

        const auto s = warpr::tests::scan (r1, 0);
        std::printf ("        (%.0f s, peak %.3f, %d state captures during the render)\n", seconds, (double) s.peak, captures);
        check (s.allFinite && warpr::tests::scan (r1, 1).allFinite && s.peak > 0.01f && s.peak <= 1.0f, "storm render is finite, audible and within the safety knee");
        check (identical (r1, r3), "two fresh runs are bit-identical");
        check (identical (r1, r2), "capturing the state between blocks does not change the audio by a single bit");
    }
}

//==============================================================================
int runPhaseD9Tests (bool full)
{
    failures = 0;
    printedDetails = 0;
    std::printf ("\n=== Phase D9: state container, embedded sample, presets, settings, regression ===\n");

    testLayout();
    testCodec();
    testRoundTripMatrix();
    testParamsAndUiState();
    testAudioEquality();
    testCompatibility();
    testFuzz (full);
    testPresets();
    testRevisionsAndAutomation();
    testSettings();
    testStateSizes (full);
    testSoak (full);

    std::printf ("[D9] %d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures;
}

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
