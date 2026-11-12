#include "Backend/UiBackendImpl.h"
#include "DSP/FilterDesign.h"
#include "DSP/ShaperPresetLibrary.h"

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

    auto* curves = new juce::DynamicObject();
    curves->setProperty ("A", parseOr (m.shaperCurveJson[0], UiModel::defaultShaperCurveJson(), true));
    curves->setProperty ("B", parseOr (m.shaperCurveJson[1], UiModel::defaultShaperCurveJson(), true));
    root->setProperty ("shaperCurves", juce::var (curves));

    return juce::JSON::toString (juce::var (root), true);
}

void UiBackendImpl::setLfoCount (int n)
{
    const juce::ScopedLock sl (modelLock);
    model.lfoCount = juce::jlimit (1, UiModel::maxLfos, n);
    // D8: publish lfoCount to the LfoEngine (atomic int).
}

void UiBackendImpl::setLfoShapeJson (int lfoIndex, const juce::String& json)
{
    if (lfoIndex < 0 || lfoIndex >= UiModel::maxLfos)
        return;

    const juce::ScopedLock sl (modelLock);
    model.lfoShapeJson[(size_t) lfoIndex] = json;
    // D8: parse -> LfoTableBuilder -> LockFreeSwap<LfoShapeTable>::publish.
}

void UiBackendImpl::setModMatrixJson (const juce::String& json)
{
    const juce::ScopedLock sl (modelLock);
    model.modMatrixJson = json;
    // D8: parse + validate destinations -> ModMatrixSnapshot -> LockFreeSwap::publish.
}

//==============================================================================
// Shaper curves
//==============================================================================
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

    // Asymmetric presets are only audible as designed in Asymmetric mode (architecture ambiguity A6): switch the
    // parameter first, drop the dirty flag it raises, and rebuild exactly once.
    if (warpr::shaperpresets::isAsymmetric (preset))
    {
        setChoiceParam (kSymmetryId, 1);
        symmetryDirty.store (false, std::memory_order_release);
    }

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
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "WARPR", message);
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

//==============================================================================
// Filter response
//==============================================================================
void UiBackendImpl::computeFilterResponse (float* dbOut, int n)
{
    if (dbOut == nullptr || n <= 0)
        return;

    // Raw (denormalised) values straight from the atomics: Choice = index, Float = real units.  Without an
    // APVTS (class-level tests) the parameter defaults are used.
    auto raw = [this] (const char* id, double fallback)
    {
        if (apvts != nullptr)
            if (auto* a = apvts->getRawParameterValue (id))
                return static_cast<double> (a->load());

        return fallback;
    };

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

void UiBackendImpl::presetStep (int)       {}   // D9: PresetManager
void UiBackendImpl::presetSaveDialog()     {}   // D9
void UiBackendImpl::presetLoadDialog()     {}   // D9
void UiBackendImpl::openSettings()         {}   // D9/D10: native settings dialog

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

    bumpRevision (Revision::uiState);
    bumpRevision (Revision::sample);
    bumpRevision (Revision::shaperCurves);
    bumpRevision (Revision::filterResponse);
    bumpRevision (Revision::preset);
}
