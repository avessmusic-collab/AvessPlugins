#include "Backend/UiBackendImpl.h"

using warpr::UiModel;

namespace
{
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
void UiBackendImpl::setShaperCurveJson (const juce::String& curveId, const juce::String& json)
{
    const int curve = UiModel::curveIndex (curveId);
    if (curve < 0)
        return;

    {
        const juce::ScopedLock sl (modelLock);
        model.shaperCurveJson[(size_t) curve] = json;
    }

    // Echo avoidance (contract): UI-originated edits NEVER bump Revision::shaperCurves.
    // D4: build LUT + antiderivative, swap into the ShaperEngine, set shaperPreset -> Custom.
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

    {
        const juce::ScopedLock sl (modelLock);
        model.shaperCurveJson[(size_t) curve] = UiModel::defaultShaperCurveJson();
    }

    bumpRevision (Revision::shaperCurves);       // reset is not UI-echo: the editor must re-emit
    // D4: rebuild + swap the LUT, set shaperPreset -> Custom.
}

void UiBackendImpl::shaperNormalize (const juce::String&)
{
    // NOT IMPLEMENTED until D4: normalising needs the curve evaluator (y *= 1/max|y|).
    // Intentionally does nothing and does not bump the revision.
}

//==============================================================================
// Sample
//==============================================================================
void UiBackendImpl::loadSampleFile (const juce::File&)
{
    // NOT IMPLEMENTED until D1 (SampleLoader on a ThreadPool + SampleStore publication).
    // Does nothing and does not bump Revision::sample.
}

WarprSampleDisplay UiBackendImpl::getSampleDisplay()
{
    return {};      // nothing loaded -> editor shows NO SAMPLE (real until D1)
}

//==============================================================================
// Filter response
//==============================================================================
void UiBackendImpl::computeFilterResponse (float* dbOut, int n)
{
    // NOT IMPLEMENTED until D5 (FilterResponse::magnitudeDb from the real coefficient code).
    // There is no filter DSP yet, so report a flat 0 dB line rather than inventing a curve.
    for (int i = 0; i < n; ++i)
        dbOut[i] = 0.0f;
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
    }

    bumpRevision (Revision::uiState);
    bumpRevision (Revision::sample);
    bumpRevision (Revision::shaperCurves);
    bumpRevision (Revision::filterResponse);
    bumpRevision (Revision::preset);
}
