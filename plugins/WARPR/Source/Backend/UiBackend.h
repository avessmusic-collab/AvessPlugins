#pragma once
/*
  UiBackend.h - what the WebView editor needs from the processor.

  Interface moved verbatim from the v9 mockup editor header.  Every method except vizFifo()
  is called on the MESSAGE thread only.  The implementation is UiBackendImpl (stubbed in
  Stage 1; each method is filled by the DSP phase named in UiBackendImpl.cpp).
*/

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

#include "DSP/VizFifo.h"

struct WarprSampleDisplay
{
    juce::String name;                       // "" = nothing loaded (editor shows NO SAMPLE)
    int numChannels = 0;
    double sampleRate = 0;
    juce::int64 numSamples = 0;
    std::vector<float> peaksMinMax;          // [min0,max0,min1,max1,...] in -1..1, <= 2048 pairs, mono mix
};

class WarprUiBackend
{
public:
    enum class Revision { uiState, sample, shaperCurves, filterResponse, preset };

    virtual ~WarprUiBackend() = default;

    // sounding voices' read positions for the waveform playheads (wait-free snapshot; message thread).
    // Returns the number of entries written to `out` (<= maxOut).
    virtual int getPlayheads (WarprVizFrame::Voice* out, int maxOut) = 0;

    virtual WarprVizFifo& vizFifo() = 0;                                    // audio thread pushes, editor pops

    // monotonically increasing counters; the editor timer re-emits the matching event when one changes.
    // uiState/sample/preset: bump on setStateInformation + preset load + sample load.
    // shaperCurves: bump ONLY for non-UI-originated changes (preset param, reset, normalize, state load)
    //   - never for setShaperCurveJson, or the UI would receive an echo mid-drag.
    // filterResponse: bump when any filter parameter or the filter state changes.
    virtual uint32_t revision (Revision) const = 0;

    // non-parameter state (see parameter-spec.md "Non-Parameter State")
    virtual juce::String getUiStateJson() = 0;                              // {lfoCount,lfoShapes[],modMatrix[],shaperCurves{A,B}}
    virtual void setLfoCount (int n) = 0;
    virtual void setLfoShapeJson (int index, const juce::String& json) = 0;
    virtual void setModMatrixJson (const juce::String& json) = 0;
    virtual void setShaperCurveJson (const juce::String& curveId, const juce::String& json) = 0;
    virtual juce::String getShaperCurveJson (const juce::String& curveId) = 0;
    virtual void shaperReset (const juce::String& curveId) = 0;
    virtual void shaperNormalize (const juce::String& curveId) = 0;

    // sample (decode OFF the audio thread inside; publish by bumping Revision::sample)
    virtual void loadSampleFile (const juce::File& file) = 0;
    virtual WarprSampleDisplay getSampleDisplay() = 0;

    // waveform zoom: min/max pairs for the sub-range [startNorm, endNorm] of the sample (message
    // thread; up to numPairs pairs, capped at 2048).  Default: no data (test backends).
    virtual std::vector<float> getRangePeaks (double startNorm, double endNorm, int numPairs)
    {
        juce::ignoreUnused (startNorm, endNorm, numPairs);
        return {};
    }

    // real filter magnitude response: fills n log-spaced points 20 Hz..20 kHz in dB from the actual filter DSP
    virtual void computeFilterResponse (float* dbOut, int n) = 0;

    // presets
    virtual juce::String getPresetName() = 0;
    virtual void presetStep (int delta) = 0;
    virtual void presetSaveDialog() = 0;
    virtual void presetLoadDialog() = 0;
    virtual void openSettings() = 0;
};
