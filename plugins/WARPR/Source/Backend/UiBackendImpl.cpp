#include "Backend/UiBackendImpl.h"
#include "Backend/SettingsDialog.h"
#include "DSP/FilterDesign.h"
#include "DSP/ShaperPresetLibrary.h"
#include "PluginIdentity.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

using warpr::ShaperCurve;
using warpr::UiModel;

namespace
{
    constexpr const char* kPresetId   = "shaperPreset";
    constexpr const char* kSymmetryId = "shaperSymmetry";
    constexpr const char* kSelectId   = "shaperCurveSelect";

    // The 8 filter parameters whose change invalidates the response display (Revision::filterResponse).
    constexpr const char* kFilterIds[8] = { "filterEnable", "filterType", "filterCutoff", "filterResonance",
                                            "filterDrive", "filterMorph", "filterKeyTrack", "filterMix" };

    /** Round trip through the canonical text: the curve the DSP builds from is EXACTLY what the model stores
        (and what a state reload will rebuild). */
    ShaperCurve canonical (const ShaperCurve& c)
    {
        ShaperCurve out;
        return ShaperCurve::fromJson (c.toJson(), out) ? out : ShaperCurve::identity();
    }

    /** Parses `json`; if it is not valid JSON of the expected kind, parses `fallback` instead. */
    juce::var parseOr (const juce::String& json, const juce::String& fallback, bool wantObject)
    {
        auto v = json.isNotEmpty() ? juce::JSON::parse (json) : juce::var();
        const bool ok = wantObject ? v.getDynamicObject() != nullptr : v.isArray();
        return ok ? v : juce::JSON::parse (fallback);
    }
}

//==============================================================================
// Non-parameter state
//==============================================================================
juce::String UiBackendImpl::getUiStateJson()
{
    const auto m = copyModel();

    auto* root = new juce::DynamicObject();
    root->setProperty ("lfoCount", m.lfoCount);

    juce::Array<juce::var> shapes;
    for (int i = 0; i < m.lfoCount; ++i)
        shapes.add (parseOr (m.lfoShapeJson[(size_t) i], UiModel::defaultLfoShapeJson(), true));
    root->setProperty ("lfoShapes", shapes);

    root->setProperty ("modMatrix", parseOr (m.modMatrixJson, UiModel::defaultModMatrixJson(), false));

    // D11: the slice map document, exactly as the UI sent it (an unset / non-object document reads as "off")
    root->setProperty ("sliceMap", parseOr (m.sliceMapJson, warpr::SliceMap::defaultJson(), true));
    root->setProperty ("grainPosKey", m.grainPosKey);

    auto* curves = new juce::DynamicObject();
    curves->setProperty ("A", parseOr (m.shaperCurveJson[0], UiModel::defaultShaperCurveJson(), true));
    curves->setProperty ("B", parseOr (m.shaperCurveJson[1], UiModel::defaultShaperCurveJson(), true));
    root->setProperty ("shaperCurves", juce::var (curves));

    return juce::JSON::toString (juce::var (root), true);
}

void UiBackendImpl::setLfoCount (int n)
{
    int count = 1;

    {
        const juce::ScopedLock sl (modelLock);
        model.lfoCount = juce::jlimit (1, UiModel::maxLfos, n);
        count = model.lfoCount;
    }

    if (modEngine != nullptr)
        modEngine->setLfoCount (count);              // atomic; LFOs >= count are idle, their routes ignored
}

void UiBackendImpl::setLfoShapeJson (int lfoIndex, const juce::String& json)
{
    if (lfoIndex < 0 || lfoIndex >= UiModel::maxLfos)
        return;

    {
        const juce::ScopedLock sl (modelLock);
        model.lfoShapeJson[(size_t) lfoIndex] = json;           // verbatim: this is what state and the UI get back
    }

    publishLfoTable (lfoIndex, json);                // parse + build on THIS thread, publish through the lock-free swap
}

void UiBackendImpl::setModMatrixJson (const juce::String& json)
{
    {
        const juce::ScopedLock sl (modelLock);
        model.modMatrixJson = json;
    }

    if (modEngine != nullptr)
        modEngine->publishMatrix (warpr::ModMatrixSnapshot::fromJson (json));
}

void UiBackendImpl::setSliceMapJson (const juce::String& json)
{
    {
        const juce::ScopedLock sl (modelLock);
        model.sliceMapJson = json;                           // verbatim: state, presets and getUiStateJson serve exactly this
    }

    // Compile on THIS thread (parse + validate + cap 64), publish through the lock-free swap.  Malformed = slicing off.
    // No revision bump: the UI that sent the edit already shows it (echo avoidance, like setModMatrixJson).
    if (modEngine != nullptr)
        modEngine->publishSliceMap (warpr::SliceMap::compile (json));
}

void UiBackendImpl::setGrainPosKey (bool on)
{
    {
        const juce::ScopedLock sl (modelLock);
        model.grainPosKey = on;
    }

    if (synthRef != nullptr)
        synthRef->setGrainPosKey (on);       // NEW notes pick it up; no revision bump (echo avoidance)
}

double UiBackendImpl::raw (const char* id, double fallback) const
{
    if (apvts != nullptr)
        if (auto* p = apvts->getRawParameterValue (id))
            return static_cast<double> (p->load());

    return fallback;
}

bool UiBackendImpl::computeSliceEdges (int count, bool byTransients, std::vector<double>& out)
{
    out.clear();

    if (sampleStore == nullptr)
        return false;

    const auto sample = sampleStore->latest();

    if (sample == nullptr || sample->level (0).numFrames <= 0)
        return false;


    const double start = juce::jlimit (0.0, 1.0, raw ("sampleStart", 0.0));
    const double end = juce::jlimit (0.0, 1.0, raw ("sampleEnd", 1.0));
    const auto& l0 = sample->level (0);

    out = warpr::autoSliceEdges (l0.channel (0), sample->numChannels > 1 ? l0.channel (1) : nullptr, l0.numFrames,
                                 sample->fileSampleRate, start, end, count, byTransients);
    return true;
}

void UiBackendImpl::attachModEngine (warpr::ModEngine* engine)
{
    modEngine = engine;
    publishAllModulation();
}

void UiBackendImpl::publishLfoTable (int index, const juce::String& json)
{
    if (modEngine == nullptr || index < 0 || index >= UiModel::maxLfos)
        return;

    // Serialised so the last publication for an index is the newest document; an unchanged document is not rebuilt
    // (a state load republishes all ten shapes, and the 16384-cell build is the expensive part).
    const juce::ScopedLock sl (lfoPublishLock);

    if (lfoPublished[(size_t) index] && publishedLfoJson[(size_t) index] == json)
        return;

    warpr::LfoShapeDesc desc;

    if (! warpr::LfoShapeDesc::fromJson (json, desc))
        desc = warpr::LfoShapeDesc::defaultShape();         // "" (never edited) or a document that is not an object

    modEngine->publishTable (index, warpr::LfoTableBuilder::build (desc));
    publishedLfoJson[(size_t) index] = json;
    lfoPublished[(size_t) index] = true;
}

void UiBackendImpl::publishAllModulation()
{
    if (modEngine == nullptr)
        return;

    const UiModel m = copyModel();

    for (int i = 0; i < UiModel::maxLfos; ++i)
        publishLfoTable (i, m.lfoShapeJson[(size_t) i]);

    modEngine->publishMatrix (warpr::ModMatrixSnapshot::fromJson (m.modMatrixJson));
    modEngine->publishSliceMap (warpr::SliceMap::compile (m.sliceMapJson));        // D11: a state / preset load restores slicing

    if (synthRef != nullptr)
        synthRef->setGrainPosKey (m.grainPosKey);                                  // POS-KEY grain mode travels with the state too
    modEngine->setLfoCount (m.lfoCount);
}

//==============================================================================
// Shaper curves
//==============================================================================
UiBackendImpl::UiBackendImpl() = default;

UiBackendImpl::~UiBackendImpl()
{
    if (apvts != nullptr)
    {
        apvts->removeParameterListener (kPresetId, &shaperListener);
        apvts->removeParameterListener (kSymmetryId, &shaperListener);

        for (const char* id : kFilterIds)
            apvts->removeParameterListener (id, &filterListener);
    }
}

void UiBackendImpl::ShaperParamListener::parameterChanged (const juce::String& parameterID, float)
{
    // Any thread (host automation can call this on the audio thread): atomics only.
    if (parameterID == kPresetId)
        owner.presetDirty.store (true, std::memory_order_release);
    else if (parameterID == kSymmetryId)
        owner.symmetryDirty.store (true, std::memory_order_release);
}

void UiBackendImpl::attachApvts (juce::AudioProcessorValueTreeState* apvtsToUse)
{
    apvts = apvtsToUse;

    if (apvts != nullptr)
    {
        apvts->addParameterListener (kPresetId, &shaperListener);
        apvts->addParameterListener (kSymmetryId, &shaperListener);

        for (const char* id : kFilterIds)
            apvts->addParameterListener (id, &filterListener);
    }
}

void UiBackendImpl::attachShaper (warpr::ShaperEngine* engine)
{
    shaperEngine = engine;
    rebuildShaper();
}

warpr::ShaperLutSet::Ptr UiBackendImpl::getShaperLut() const
{
    const juce::ScopedLock sl (shaperLock);
    return lastLut;
}

//------------------------------------------------------------------------------
int UiBackendImpl::currentChoice (const char* id) const
{
    if (apvts != nullptr)
        if (auto* raw = apvts->getRawParameterValue (id))
            return juce::roundToInt (raw->load());

    return -1;
}

bool UiBackendImpl::isSymmetric() const
{
    const int v = currentChoice (kSymmetryId);
    return v <= 0;                                   // 0 = Symmetric (also the default without an APVTS)
}

void UiBackendImpl::setChoiceParam (const char* id, int index)
{
    if (apvts == nullptr || currentChoice (id) == index)
        return;

    if (auto* p = apvts->getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
        p->endChangeGesture();
    }
}

void UiBackendImpl::flipPresetToCustom()
{
    setChoiceParam (kPresetId, warpr::shaperpresets::kCustomIndex);
}

/** Builds both curves into one LUT set and publishes it.  Serialised by shaperLock so the last publication is
    always the newest model state. */
void UiBackendImpl::rebuildShaper()
{
    const juce::ScopedLock build (shaperLock);

    std::array<ShaperCurve, 2> copy;
    {
        const juce::ScopedLock sl (modelLock);
        copy = shaperCurves;
    }

    auto set = warpr::ShaperLutSet::build (copy[0], copy[1], isSymmetric());
    lastLut = set;

    if (shaperEngine != nullptr)
        shaperEngine->publish (set);
}

/** Model + parsed twin updated together (canonical text). */
void UiBackendImpl::storeCurve (int curve, const ShaperCurve& c)
{
    const juce::ScopedLock sl (modelLock);
    shaperCurves[(size_t) curve] = c;
    model.shaperCurveJson[(size_t) curve] = c.toJson();
}

void UiBackendImpl::setShaperCurveJson (const juce::String& curveId, const juce::String& json)
{
    const int curve = UiModel::curveIndex (curveId);
    if (curve < 0)
        return;

    ShaperCurve parsed;
    if (! ShaperCurve::fromJson (json, parsed))
        return;                                      // malformed / out-of-contract document: the model is left untouched

    storeCurve (curve, parsed);
    rebuildShaper();

    // Echo avoidance (contract): UI-originated edits NEVER bump Revision::shaperCurves.
    // Editing a point makes the curve user-defined: shaperPreset -> Custom (host gesture bracketed).
    flipPresetToCustom();
}

juce::String UiBackendImpl::getShaperCurveJson (const juce::String& curveId)
{
    const int curve = UiModel::curveIndex (curveId);
    if (curve < 0)
        return UiModel::defaultShaperCurveJson();

    const juce::ScopedLock sl (modelLock);
    const auto& stored = model.shaperCurveJson[(size_t) curve];
    return stored.isNotEmpty() ? stored : UiModel::defaultShaperCurveJson();
}

void UiBackendImpl::shaperReset (const juce::String& curveId)
{
    const int curve = UiModel::curveIndex (curveId);
    if (curve < 0)
        return;

    storeCurve (curve, ShaperCurve::identity());
    rebuildShaper();
    bumpRevision (Revision::shaperCurves);           // reset is not UI-echo: the editor must re-emit
    flipPresetToCustom();
}

void UiBackendImpl::shaperNormalize (const juce::String& curveId)
{
    const int curve = UiModel::curveIndex (curveId);
    if (curve < 0)
        return;

    ShaperCurve c;
    {
        const juce::ScopedLock sl (modelLock);
        c = shaperCurves[(size_t) curve];
    }

    // max |y| of the EFFECTIVE curve (the odd mirror of the x >= 0 half in Symmetric mode), sampled on the LUT grid.
    // Both interpolators are monotone per segment, so the maximum sits on a control point and the grid sees it.
    const ShaperCurve::Evaluator ev (c, isSymmetric());
    double peak = 0.0;

    for (int i = 0; i <= warpr::ShaperLutSet::kCells; ++i)
        peak = std::max (peak, std::abs (ev (-1.0 + (double) i * warpr::ShaperLutSet::kCell)));

    if (! (peak > 1.0e-9))
        return;                                      // a flat zero curve cannot be normalised: nothing changes

    c.scaleY (1.0 / peak);
    storeCurve (curve, canonical (c));
    rebuildShaper();
    bumpRevision (Revision::shaperCurves);
    flipPresetToCustom();
}

/** shaperPreset changed to a loadable preset: regenerate the curve selected by shaperCurveSelect. */
void UiBackendImpl::applyShaperPreset (int preset)
{
    const int curve = juce::jlimit (0, 1, currentChoice (kSelectId));
    const auto c = canonical (warpr::shaperpresets::generate (preset));

    // Asymmetric presets are only audible as designed in Asymmetric mode (architecture ambiguity A6).
    // The backend does NOT write shaperSymmetry here: a parameter must never rewrite another parameter,
    // or hosts lose automation independence (auval: "parameter did not retain set value when initialized").
    // The UI flips SYM->ASYM itself when the USER picks an asymmetric preset; a host automating the preset
    // alone gets the current symmetry applied to the asymmetric curve, documented behaviour.

    storeCurve (curve, c);
    rebuildShaper();
    bumpRevision (Revision::shaperCurves);
}

void UiBackendImpl::servicePending()
{
    if (presetDirty.exchange (false, std::memory_order_acq_rel))
    {
        const int preset = currentChoice (kPresetId);

        if (warpr::shaperpresets::isLoadable (preset))
            applyShaperPreset (preset);
    }

    if (symmetryDirty.exchange (false, std::memory_order_acq_rel))
        rebuildShaper();
}

//==============================================================================
// Sample
//==============================================================================
void UiBackendImpl::loadSampleFile (const juce::File& file)
{
    if (sampleStore == nullptr)
        return;

    // Decode + mip build on the SampleStore loader thread, publish through the lock-free swap.
    // The callback runs on the loader thread: it only bumps an atomic or queues a message-thread alert.
    sampleStore->loadFileAsync (file, [this] (bool ok, const juce::String& error)
    {
        if (ok)
        {
            bumpRevision (Revision::sample);
            return;
        }

        // Architecture "Error handling": the previous sample is kept and the user is told why.
        const juce::String message = "The sample could not be loaded:\n" + error;

        juce::MessageManager::callAsync ([message]
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, warpr::identity::productName, message);
        });
    });
}

WarprSampleDisplay UiBackendImpl::getSampleDisplay()
{
    WarprSampleDisplay display;

    if (sampleStore == nullptr)
        return display;

    const auto sample = sampleStore->latest();

    if (sample == nullptr)
        return display;

    display.name = sample->name;
    display.numChannels = sample->numChannels;
    display.sampleRate = sample->fileSampleRate;
    display.numSamples = sample->numFrames;
    display.peaksMinMax = sample->peaksMinMax;
    return display;
}

std::vector<float> UiBackendImpl::getRangePeaks (double startNorm, double endNorm, int numPairs)
{
    // Waveform zoom (message thread).  The SampleData::Ptr is ref-counted and immutable, so
    // reading level 0 here is safe even if a new sample is published concurrently.
    std::vector<float> out;

    if (sampleStore == nullptr)
        return out;

    const auto sample = sampleStore->latest();

    if (sample == nullptr)
        return out;

    const auto& l0 = sample->level (0);
    const int frames = l0.numFrames;

    if (frames <= 0)
        return out;

    const double a = juce::jlimit (0.0, 1.0, juce::jmin (startNorm, endNorm));
    const double b = juce::jlimit (0.0, 1.0, juce::jmax (startNorm, endNorm));
    const int f0 = juce::jlimit (0, frames - 1, (int) std::floor (a * frames));
    const int f1 = juce::jlimit (f0 + 1, frames, (int) std::ceil (b * frames));
    const int span = f1 - f0;
    const int pairs = juce::jlimit (1, warpr::SampleData::maxPeakPairs, juce::jmin (numPairs, span));

    out.resize ((size_t) pairs * 2);

    const float* c0 = l0.channel (0);
    const float* c1 = sample->numChannels > 1 ? l0.channel (1) : nullptr;

    for (int p = 0; p < pairs; ++p)
    {
        const int i0 = f0 + (int) (((juce::int64) p * span) / pairs);
        const int i1 = juce::jmax (i0 + 1, f0 + (int) (((juce::int64) (p + 1) * span) / pairs));
        float mn = 1.0e9f, mx = -1.0e9f;

        for (int i = i0; i < i1 && i < f1; ++i)
        {
            const float v = c1 != nullptr ? 0.5f * (c0[i] + c1[i]) : c0[i];
            mn = juce::jmin (mn, v);
            mx = juce::jmax (mx, v);
        }

        out[(size_t) p * 2]     = mn;
        out[(size_t) p * 2 + 1] = mx;
    }

    return out;
}

//==============================================================================
// Processed-waveform preview
//==============================================================================
namespace
{
    /** Everything one preview run needs (built once per computeProcessedPeaks call). */
    struct PreviewCtx
    {
        const warpr::SampleData* sd = nullptr;
        const float* c0 = nullptr;
        const float* c1 = nullptr;            // nullptr for a mono sample
        warpr::RegionFold rg;
        warpr::WarpSpan warp;
        warpr::ShaperSpan shaper;
        int regionFirst = 0, regionLast = 0;  // integer frames [first, last) the voices can play

        float rawMono (int i) const noexcept  { return c1 != nullptr ? 0.5f * (c0[i] + c1[i]) : c0[i]; }
    };

    constexpr int kPreviewChunk = 32;         // the voice's chunk size

    /** Writes the mono mix of frames [a, b) after the warp + shaper stages into out[0 .. b - a).  Frames outside the
        voices' region are the raw sample.  The oscillator is restarted at the first processed frame (fresh warp /
        shaper state, cycle clock seeded from the position), then runs chunk by chunk exactly like a voice at 1x. */
    void renderPreviewRun (const PreviewCtx& cx, warpr::UnisonOsc& osc, int a, int b, float* out) noexcept
    {
        const int pa = std::max (a, cx.regionFirst);
        const int pb = std::min (b, cx.regionLast);

        if (pb <= pa)
        {
            for (int i = a; i < b; ++i)
                out[i - a] = cx.rawMono (i);

            return;
        }

        for (int i = a; i < pa; ++i)
            out[i - a] = cx.rawMono (i);

        for (int i = pb; i < b; ++i)
            out[i - a] = cx.rawMono (i);

        osc.start (cx.rg, false, 0, cx.warp.Lc, 1u);
        osc.pos = (double) pa;
        osc.vpos = osc.pos;
        osc.vdir = osc.dir;

        const double cyc = (osc.pos - cx.rg.lo) / cx.warp.Lc;
        osc.warp.reset (cyc - std::floor (cyc));
        osc.shaper.idle();

        float busL[kPreviewChunk], busR[kPreviewChunk];

        for (int i = pa; i < pb; i += kPreviewChunk)
        {
            const int n = std::min (kPreviewChunk, pb - i);
            std::fill_n (busL, n, 0.0f);
            std::fill_n (busR, n, 0.0f);

            osc.render (*cx.sd, cx.rg, 0, 1.0, 1.0, 1, 1.0f, 1.0f, 1.0f, 1.0f, busL, busR, n, &cx.warp, &cx.shaper, nullptr);

            for (int k = 0; k < n; ++k)
            {
                const float v = cx.c1 != nullptr ? 0.5f * (busL[k] + busR[k]) : busL[k];
                out[i - a + k] = std::isfinite (v) ? v : 0.0f;          // a voice would be killed by the guard; the preview shows 0
            }
        }
    }
}

int UiBackendImpl::computeProcessedPeaks (float* minMaxInterleaved, int pairs, double startNorm, double endNorm)
{
    // Message thread.  A dedicated UnisonOsc (the same oscillator, warp stage and shaper stage a voice runs) is restarted
    // for every run, so the result depends only on the parameters, the curve set and the sample: deterministic.
    if (minMaxInterleaved == nullptr || pairs <= 0)
        return 0;

    std::fill_n (minMaxInterleaved, (size_t) pairs * 2, 0.0f);

    if (sampleStore == nullptr)
        return 0;

    const auto sample = sampleStore->latest();

    if (sample == nullptr || sample->level (0).numFrames <= 0)
        return 0;

    const auto& l0 = sample->level (0);
    const int frames = l0.numFrames;

    // same span / bucket arithmetic as getRangePeaks
    const double a = juce::jlimit (0.0, 1.0, juce::jmin (startNorm, endNorm));
    const double b = juce::jlimit (0.0, 1.0, juce::jmax (startNorm, endNorm));
    const int f0 = juce::jlimit (0, frames - 1, (int) std::floor (a * frames));
    const int f1 = juce::jlimit (f0 + 1, frames, (int) std::ceil (b * frames));
    const int span = f1 - f0;
    const int outPairs = juce::jlimit (1, warpr::SampleData::maxPeakPairs, juce::jmin (pairs, span));

    // ---- current parameter values (raw atomics, same clamps as WarprSynth::setBlockParams; defaults without an APVTS)

    auto rawF = [this] (const char* id, double fallback, float lo, float hi)
    {
        return juce::jlimit (lo, hi, (float) raw (id, fallback));
    };

    const bool warpEnable = raw ("warpEnable", 1.0) >= 0.5;
    const bool shaperEnable = raw ("shaperEnable", 1.0) >= 0.5;
    const int rootNote = juce::jlimit (0, 127, juce::roundToInt (raw ("rootNote", 60.0)));
    const int loopMode = juce::jlimit (0, 2, juce::roundToInt (raw ("loopMode", 0.0)));

    PreviewCtx cx;
    cx.sd = sample.get();
    cx.c0 = l0.channel (0);
    cx.c1 = sample->numChannels > 1 ? l0.channel (1) : nullptr;

    // the voices' region (sampleStart..sampleEnd, loop mode) - phase warps fold their read offsets into it.  The loop-seam
    // crossfade and the One-Shot end fade are loop-boundary treatments, not warp / shaper effects: left out of the preview.
    const auto foldMode = loopMode == 1 ? warpr::RegionFold::Mode::PingPong
                                        : (loopMode == 2 ? warpr::RegionFold::Mode::OneShot : warpr::RegionFold::Mode::Forward);
    cx.rg.configure (*sample, rawF ("sampleStart", 0.0, 0.0f, 1.0f), rawF ("sampleEnd", 1.0, 0.0f, 1.0f), foldMode);
    cx.rg.xfade = 0.0;
    cx.rg.endFade = 0.0;
    cx.regionFirst = juce::jlimit (0, frames, (int) std::ceil (cx.rg.lo));
    cx.regionLast = juce::jlimit (0, frames, (int) std::ceil (cx.rg.hi));          // frames i with i < hi

    // warp stage: the steady state of a voice's smoothed values (amount / A / B / mix * enable, ratio), 1x oversampling
    const auto lut = getShaperLut();
    const double osRate = sample->fileSampleRate;                // the preview runs one output sample per source frame
    const float warpMix = rawF ("warpMix", 0.0, 0.0f, 1.0f) * (warpEnable ? 1.0f : 0.0f);
    const float ratio = rawF ("modOscRatio", 1.0, 0.25f, 16.0f);

    cx.warp.mode = juce::jlimit (0, warpr::kNumWarpModes - 1, juce::roundToInt (raw ("warpMode", 17.0)));
    cx.warp.a0 = cx.warp.a1 = rawF ("warpAmount", 0.0, 0.0f, 1.0f);
    cx.warp.A0 = cx.warp.A1 = rawF ("warpParamA", 0.5, 0.0f, 1.0f);
    cx.warp.B0 = cx.warp.B1 = rawF ("warpParamB", 0.0, 0.0f, 1.0f);
    cx.warp.mix0 = cx.warp.mix1 = warpMix;
    cx.warp.active = warpMix > 0.0f;
    cx.warp.Lc = sample->fileSampleRate / (440.0 * std::exp2 (((double) (float) rootNote - 69.0) / 12.0));   // a voice at its root pitch
    cx.warp.fadeLen = std::max (1, (int) std::lround (0.010 * osRate));
    cx.warp.os = 1;
    cx.warp.osRate = osRate;
    cx.warp.modSource = juce::jlimit (0, 4, juce::roundToInt (raw ("modOscSource", 4.0)));
    cx.warp.ratio0 = cx.warp.ratio1 = ratio;
    cx.warp.curve = lut.get();                                   // Phase Remapping reads the curve set even with the shaper off

    const float shMix = rawF ("shaperMix", 0.0, 0.0f, 1.0f) * (shaperEnable ? 1.0f : 0.0f);
    const float shMorph = rawF ("shaperMorph", 0.0, 0.0f, 1.0f);
    cx.shaper.cur = lut.get();
    cx.shaper.prev = nullptr;
    cx.shaper.morph0 = cx.shaper.morph1 = shMorph;
    cx.shaper.mix0 = cx.shaper.mix1 = shMix;
    cx.shaper.w0 = cx.shaper.w1 = 1.0f;
    cx.shaper.active = cx.shaper.cur != nullptr && shMix > 0.0f;

    if (previewOsc == nullptr)
        previewOsc = std::make_unique<warpr::UnisonOsc>();

    auto& osc = *previewOsc;

    auto reduce = [] (const float* s, int count, float& mn, float& mx)
    {
        mn = 1.0e9f;
        mx = -1.0e9f;

        for (int i = 0; i < count; ++i)
        {
            mn = juce::jmin (mn, s[i]);
            mx = juce::jmax (mx, s[i]);
        }
    };

    if (span <= kPreviewFrameCap)
    {
        // one continuous run over the whole span (stateful modes behave as in a sounding voice)
        std::vector<float> stream ((size_t) span);
        renderPreviewRun (cx, osc, f0, f1, stream.data());

        for (int p = 0; p < outPairs; ++p)
        {
            const int i0 = (int) (((juce::int64) p * span) / outPairs);
            const int i1 = juce::jmax (i0 + 1, (int) (((juce::int64) (p + 1) * span) / outPairs));
            float mn, mx;
            reduce (stream.data() + i0, juce::jmin (i1, span) - i0, mn, mx);
            minMaxInterleaved[(size_t) p * 2]     = mn;
            minMaxInterleaved[(size_t) p * 2 + 1] = mx;
        }

        return outPairs;
    }

    // Over the cap: every output pair gets its own window of real processed frames (centred in its bucket, at most
    // cap / pairs frames, fresh state per window).  Nothing is interpolated or invented; the work is decimated.
    const int windowMax = std::max (64, kPreviewFrameCap / outPairs);
    std::vector<float> scratch ((size_t) windowMax);

    for (int p = 0; p < outPairs; ++p)
    {
        const int i0 = f0 + (int) (((juce::int64) p * span) / outPairs);
        const int i1 = juce::jmin (f1, juce::jmax (i0 + 1, f0 + (int) (((juce::int64) (p + 1) * span) / outPairs)));
        const int len = std::min (i1 - i0, windowMax);
        const int w0 = i0 + ((i1 - i0) - len) / 2;

        renderPreviewRun (cx, osc, w0, w0 + len, scratch.data());

        float mn, mx;
        reduce (scratch.data(), len, mn, mx);
        minMaxInterleaved[(size_t) p * 2]     = mn;
        minMaxInterleaved[(size_t) p * 2 + 1] = mx;
    }

    return outPairs;
}

//==============================================================================
// Filter response
//==============================================================================
void UiBackendImpl::computeFilterResponse (float* dbOut, int n)
{
    if (dbOut == nullptr || n <= 0)
        return;

    // Raw (denormalised) values straight from the atomics: Choice = index, Float = real units.  Without an
    // APVTS (class-level tests) the parameter defaults are used.

    // Same coefficient generators as the voices (FilterDesign); key tracking is note-dependent and not drawn.
    warpr::FilterResponse::magnitudeDbLogSpaced (juce::roundToInt (raw ("filterType", 3.0)),
                                                 raw ("filterCutoff", 20000.0),
                                                 raw ("filterResonance", 0.0),
                                                 raw ("filterDrive", 0.0),
                                                 raw ("filterMorph", 0.0),
                                                 responseSampleRate.load (std::memory_order_relaxed),
                                                 dbOut, n);
}

//==============================================================================
// Presets / settings
//==============================================================================
juce::String UiBackendImpl::getPresetName()
{
    const juce::ScopedLock sl (modelLock);
    return model.presetName;
}

void UiBackendImpl::attachStateHost (StateCapture capture, StateApply apply)
{
    stateCapture = std::move (capture);
    stateApply = std::move (apply);
}

void UiBackendImpl::attachSettings (SettingsGetter getter, SettingsSetter setter)
{
    settingsGetter = std::move (getter);
    settingsSetter = std::move (setter);
}

void UiBackendImpl::setErrorReporter (ErrorReporter reporter)
{
    errorReporter = std::move (reporter);
}

void UiBackendImpl::reportError (const juce::String& message)
{
    if (errorReporter)
    {
        errorReporter (message);
        return;
    }

    juce::MessageManager::callAsync ([message]
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, warpr::identity::productName, message);
    });
}

void UiBackendImpl::quietlySetPresetName (const juce::String& name)
{
    const juce::ScopedLock sl (modelLock);
    model.presetName = name;
}

void UiBackendImpl::setPresetName (const juce::String& name)
{
    quietlySetPresetName (name);
    bumpRevision (Revision::preset);                  // the editor's "warprPreset" event
}

//------------------------------------------------------------------------------
bool UiBackendImpl::savePresetToFile (const juce::File& requested)
{
    if (! stateCapture)
    {
        reportError ("Presets are not available.");
        return false;
    }

    const auto ext = warpr::PresetManager::extension();
    const auto file = requested.hasFileExtension (ext)
                        ? requested
                        : requested.getSiblingFile (requested.getFileName() + ext);     // "my.preset" -> "my.preset.void"

    const auto name = file.getFileNameWithoutExtension();
    const auto previous = getPresetName();

    quietlySetPresetName (name);                      // the saved state carries its own name
    const auto block = stateCapture();

    juce::String error;

    if (! warpr::PresetManager::writeFile (file, block, error))
    {
        quietlySetPresetName (previous);
        reportError ("The preset could not be saved:\n" + error);
        return false;
    }

    bumpRevision (Revision::preset);
    return true;
}

bool UiBackendImpl::saveUserPreset (const juce::String& name)
{
    if (presetManager == nullptr)
    {
        reportError ("Presets are not available.");
        return false;
    }

    return savePresetToFile (presetManager->fileForName (name));
}

bool UiBackendImpl::applyPresetBytes (const juce::MemoryBlock& block, const juce::String& displayName, juce::String& error)
{
    if (! stateApply)
    {
        error = "Presets are not available.";
        return false;
    }

    const auto outcome = stateApply (block.getData(), block.getSize());

    if (outcome == warpr::state::LoadOutcome::rejected)
    {
        error = "\"" + displayName + "\" is not a valid " + juce::String (warpr::identity::productName) + " preset.";
        return false;
    }

    setPresetName (displayName);                      // after adoptModel: a renamed file shows the name the user sees

    if (outcome == warpr::state::LoadOutcome::loadedDefaultSampleAfterCorruptBlob)
        reportError ("The sample embedded in \"" + displayName + "\" could not be read. The default wave is used instead.");

    return true;
}

bool UiBackendImpl::loadPresetFile (const juce::File& file)
{
    juce::MemoryBlock block;
    juce::String error;

    if (! warpr::PresetManager::readFile (file, block, error) || ! applyPresetBytes (block, file.getFileNameWithoutExtension(), error))
    {
        reportError ("The preset could not be loaded:\n" + error);
        return false;
    }

    return true;
}

bool UiBackendImpl::loadPresetEntry (const warpr::PresetEntry& entry)
{
    juce::MemoryBlock block;
    juce::String error;

    if (presetManager == nullptr)
        error = "Presets are not available.";

    if (error.isEmpty() && presetManager->read (entry, block, error) && applyPresetBytes (block, entry.name, error))
        return true;

    reportError ("The preset could not be loaded:\n" + error);
    return false;
}

void UiBackendImpl::presetStep (int delta)
{
    if (presetManager == nullptr)
        return;

    const int count = static_cast<int> (presetManager->listEntries().size());
    juce::String from = getPresetName();
    juce::String firstError;

    // An unreadable entry is skipped (otherwise stepping would be stuck on it forever); one report if nothing loads.
    for (int attempt = 0; attempt < count; ++attempt)
    {
        warpr::PresetEntry next;

        if (! presetManager->stepFrom (from, delta, next))
            return;

        juce::MemoryBlock block;
        juce::String error;

        if (presetManager->read (next, block, error) && applyPresetBytes (block, next.name, error))
            return;

        if (firstError.isEmpty())
            firstError = error;

        from = next.name;
    }

    if (firstError.isNotEmpty())
        reportError ("The preset could not be loaded:\n" + firstError);
}

void UiBackendImpl::presetSaveDialog()
{
    if (presetManager == nullptr)
    {
        reportError ("Presets are not available.");
        return;
    }

    const auto dir = presetManager->getDirectory();
    dir.createDirectory();

    const auto current = getPresetName();
    const auto initial = dir.getChildFile (warpr::PresetManager::sanitiseName (current == "Init" ? juce::String ("New Preset") : current)
                                           + warpr::PresetManager::extension());

    chooser = std::make_unique<juce::FileChooser> ("Save preset", initial, "*" + warpr::PresetManager::extension());
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto result = fc.getResult();

                              if (result != juce::File())
                                  savePresetToFile (result);
                          });
}

void UiBackendImpl::presetLoadDialog()
{
    if (presetManager == nullptr)
    {
        reportError ("Presets are not available.");
        return;
    }

    const auto dir = presetManager->getDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Load preset", dir.isDirectory() ? dir : juce::File(),
                                                   "*" + warpr::PresetManager::extension());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto result = fc.getResult();

                              if (result.existsAsFile())
                                  loadPresetFile (result);
                          });
}

//------------------------------------------------------------------------------
void UiBackendImpl::openSettings()
{
    if (! settingsGetter || ! settingsSetter)
    {
        reportError ("Settings are not available.");
        return;
    }

    if (settingsWindow == nullptr)
        settingsWindow = std::make_unique<SettingsWindow> (settingsGetter(), settingsSetter);
    else
        settingsWindow->setValues (settingsGetter());

    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}

bool UiBackendImpl::isSettingsWindowShowing() const
{
    return settingsWindow != nullptr && settingsWindow->isVisible();
}

//==============================================================================
// Processor-side API
//==============================================================================
warpr::UiModel UiBackendImpl::copyModel() const
{
    const juce::ScopedLock sl (modelLock);
    return model;
}

void UiBackendImpl::adoptModel (const warpr::UiModel& newModel)
{
    {
        const juce::ScopedLock sl (modelLock);
        model = newModel;

        // Re-parse the stored curves: valid documents are canonicalised, empty / invalid ones become the identity
        // (stored as "" = default).  The DSP always builds from the parsed twin.
        for (size_t i = 0; i < (size_t) UiModel::numCurves; ++i)
        {
            ShaperCurve c;

            if (model.shaperCurveJson[i].isNotEmpty() && ShaperCurve::fromJson (model.shaperCurveJson[i], c))
            {
                shaperCurves[i] = c;
                model.shaperCurveJson[i] = c.toJson();
            }
            else
            {
                shaperCurves[i] = ShaperCurve::identity();
                model.shaperCurveJson[i] = {};
            }
        }
    }

    // The state load itself changed shaperPreset / shaperSymmetry through the APVTS; the STORED curves win, so the
    // preset listener's pending work is dropped (the symmetry rebuild below uses the loaded parameter value).
    presetDirty.store (false, std::memory_order_release);
    symmetryDirty.store (false, std::memory_order_release);
    rebuildShaper();
    publishAllModulation();                          // D8: the loaded LFO shapes, matrix and lfoCount reach the audio thread

    bumpRevision (Revision::uiState);
    bumpRevision (Revision::sample);
    bumpRevision (Revision::shaperCurves);
    bumpRevision (Revision::filterResponse);
    bumpRevision (Revision::preset);
}
