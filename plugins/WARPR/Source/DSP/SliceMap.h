#pragma once
/*
  SliceMap - SLICE MODE (Ableton-Simpler "Slice" semantics).  Non-parameter state: the UI owns the document, the model stores
  its JSON verbatim, and the audio thread reads an immutable compiled copy published through ModEngine / LockFreeSwap.

  Wire format (JSON):
      {"on":true, "baseNote":36, "edges":[e0, e1, ..., eN]}
  edges = N+1 ascending sample fractions in [0,1]; they define N slices [e_k, e_k+1).  Slice k is triggered by MIDI note
  baseNote + k (36 = C1, Simpler's convention).  At most 64 slices (more edges are ignored).  Anything that does not parse as
  such a document - not an object, fewer than 2 edges, a non-numeric / non-finite / DEScending edge - means "slicing off".
  Values outside [0,1] are clamped.  "on" accepts true/false or a number (non-zero = on); a missing "on" is off.  A missing
  or non-numeric "baseNote" is 36; it is rounded and clamped to 0..127.  A zero-width slice (equal neighbouring edges) keeps
  its note; the voice then plays the region minimum (RegionFold: max(64 frames, 2 ms)).

  Engine semantics when the published map is ON (WarprSynth / WarprVoice):
    - a note in [baseNote, baseNote + N) starts a SLICE voice: it plays only [e_k, e_k+1) (latched at note-on, overrides the
      global sampleStart / sampleEnd for that voice), UNPITCHED (reads at the sample's original rate whatever the note;
      transpose / fineTune still apply), as a one-shot TRIGGER (key-up / pedal ignored, auto-release at the slice end) and
      with Classic playback whatever playMode says.  Warp / shaper / filter / envelope / LFO matrix apply normally.
    - any other note plays nothing.

  Also here: autoSliceEdges() - the message-thread auto-slicer behind the UI's "slice into N" (equal division, or by onsets).
*/

#include <juce_core/juce_core.h>

#include <array>
#include <memory>
#include <vector>

namespace warpr
{
    struct SliceMap
    {
        static constexpr int maxSlices = 64;

        bool on = false;
        int  baseNote = 36;
        int  count = 0;                                      // number of slices (edges used = count + 1)
        std::array<float, (size_t) maxSlices + 1> edges {};

        /** Slice index of a MIDI note, or -1 when the note is outside [baseNote, baseNote + count). */
        int sliceFor (int note) const noexcept
        {
            const int k = note - baseNote;
            return (count > 0 && k >= 0 && k < count) ? k : -1;
        }

        /** Parses `json` into `out`.  Returns true when the document is a valid slice map (its "on" flag is copied; a map with
            "on":false is valid and parsed); false = malformed, `out` is reset to the OFF state. */
        static bool parse (const juce::String& json, SliceMap& out);

        /** Always returns a map (OFF when the document is malformed or empty): ready to publish. */
        static std::unique_ptr<SliceMap> compile (const juce::String& json);

        /** The document the UI sees for a model that never had one: {"on":false,"baseNote":36,"edges":[]}. */
        static juce::String defaultJson();
    };

    //==========================================================================
    /** Auto-slice of the region [startNorm, endNorm] of a sample into `count` slices (1..64): returns count + 1 ascending
        normalised edges, edges.front() == startNorm and edges.back() == endNorm exactly.  Deterministic, allocation is fine
        (message thread).

        byTransients == false : equal division, e_k = start + (end - start) * k / count.
        byTransients == true  : offline onset detector on the mono mix 0.5 (L + R) of the region:
          1. Energy: E_j = mean(x^2) over 10 ms windows with a 2.5 ms hop (prefix sums, double precision).
          2. Onset strength: o_j = max(0, E_j - E_(j-1)) (half-wave rectified energy rise), j >= 1.  The samples that entered
             the window at step j are [a + (j-1) hop + W, a + j hop + W): the onset position is the first of them whose |x|
             reaches half the peak |x| of that 2.5 ms slice (sample-accurate for clicks, a few ms early for slow attacks).
          3. Candidates = local maxima of o (o_j > o_(j-1), o_j >= o_(j+1)) with o_j >= 5 % of the strongest rise.
          4. Peak-pick the count - 1 strongest (ties: the earlier one) subject to a minimum 30 ms distance to every already
             chosen onset, to the region start (which is always an edge) and to the region end.
          5. Fewer than count - 1 onsets found: FALL BACK to equal division for the whole result.
        `c1` may be nullptr (mono sample).  A region shorter than the window, or an empty buffer, also falls back. */
    std::vector<double> autoSliceEdges (const float* c0, const float* c1, int numFrames, double sampleRate,
                                        double startNorm, double endNorm, int count, bool byTransients);
}
