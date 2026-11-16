#pragma once
/*
  UiBackend.h - what the WebView editor needs from the processor.

  Interface moved verbatim from the v9 mockup editor header.  Every method except vizFifo()
  is called on the MESSAGE thread only.  The implementation is UiBackendImpl (stubbed in
  Stage 1; each method is filled by the DSP phase named in UiBackendImpl.cpp).
*/

#include <juce_core/juce_core.h>

#include <algorithm>
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

    // SLICE MODE (D11, DSP/SliceMap.h).  The slice map is non-parameter state: setSliceMapJson stores the document VERBATIM
    // (state, presets and getUiStateJson's "sliceMap" key serve exactly what was sent), compiles it and publishes it to the
    // audio thread.  A malformed / empty document means slicing off.  Like setModMatrixJson it never bumps a revision (no
    // echo of the UI's own edits); a state / preset load (adoptModel) bumps Revision::uiState.  Defaults: not supported.
    virtual void setSliceMapJson (const juce::String& json) { juce::ignoreUnused (json); }

    // Auto-slice of the current sample REGION [sampleStart, sampleEnd] into `count` (1..64) slices: fills `out` with count + 1
    // ascending normalised edges (front == sampleStart, back == sampleEnd).  byTransients = onset detection with an equal
    // division fallback (see autoSliceEdges).  Message thread.  Returns false (out cleared) when there is no sample.
    virtual bool computeSliceEdges (int count, bool byTransients, std::vector<double>& out)
    {
        juce::ignoreUnused (count, byTransients);
        out.clear();
        return false;
    }

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

    // PROCESSED-WAVEFORM PREVIEW (message thread only; never touches the voices or the audio thread).
    //
    // Fills `minMaxInterleaved` ([min0,max0,min1,max1,...], room for `pairs` pairs) with the min/max peaks of the
    // current sample's level-0 frames in [startNorm, endNorm) (absolute sample fractions, the getRangePeaks convention
    // and the same bucketing arithmetic) AFTER the real warp stage and shaper stage have processed them with the CURRENT
    // parameter values and shaper curve set (a mono mix of L/R, like the raw peaks).  Frames the voices never play
    // (outside the sampleStart..sampleEnd region) are passed through unprocessed.
    //
    // Returns the number of pairs written = min(pairs, 2048, frames in the span); 0 = no data (no sample / no backend
    // support).  Entries beyond the returned count (and everything when 0 is returned) are set to 0.  Deterministic:
    // equal state -> bit-identical output.  Cost is bounded: spans above kPreviewFrameCap (1.5M frames) are processed as
    // evenly spaced sub-windows, one per output pair (every pair still comes from real processed frames).  The caller
    // debounces; typical cost is a few ms per second of audio.  Default: no data (test backends).
    static constexpr int kPreviewFrameCap = 1500000;

    virtual int computeProcessedPeaks (float* minMaxInterleaved, int pairs, double startNorm, double endNorm)
    {
        juce::ignoreUnused (startNorm, endNorm);

        if (minMaxInterleaved != nullptr && pairs > 0)
            std::fill_n (minMaxInterleaved, (size_t) pairs * 2, 0.0f);

        return 0;
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
