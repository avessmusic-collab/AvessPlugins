// CHOPLINE — SliceMap (spec §4)
// Slice markers and per-chop playback bounds.
//
// The MODEL (SliceModel) lives on the message thread and stores markers in
// SECONDS (so a session-rate change never moves a slice). The four slicing
// modes all just generate markers; every mode's markers can then be edited
// manually (drag / add / delete). The AUDIO side sees an immutable
// SliceMapSnapshot (sample-frame chop bounds + declick fade lengths)
// published through SnapshotExchange.
//
// The single SENS control is mode-dependent (locked v17 design: lean slicing
// controls — one knob):
//   Transient → onset-detection sensitivity
//   Grid      → division of the sample's BPM: 1/4, 1/8, 1/16, 1/32
//   Equal     → number of slices, 2–64
//   Manual    → unused

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "SampleStore.h"

#include <array>
#include <vector>

namespace chopline::engine
{

enum class SliceMode : int
{
    transient = 0,
    grid,
    equal,
    manual
};

struct SliceMarker
{
    int id = 0;          // stable while the marker lives
    double timeSec = 0;  // position in the sample
    float fadeInMs = 2.0f;  // declick fades of the chop STARTING here (0–20 ms)
    float fadeOutMs = 2.0f;
};

/// Immutable audio-thread view: chop bounds in sample frames at session rate.
struct SliceMapSnapshot
{
    static constexpr int maxChops = 64;

    struct Chop
    {
        juce::int64 start = 0, end = 0; // [start, end) in frames
        int fadeInSamples = 0, fadeOutSamples = 0;
    };

    int numChops = 0;
    std::array<Chop, maxChops> chops {};
};

class SliceModel final
{
public:
    static constexpr float defaultFadeMs = 2.0f;
    static constexpr float maxFadeMs = 20.0f;
    static constexpr double zeroCrossWindowSec = 0.01; // ±10 ms search

    // Settings (message thread).
    SliceMode mode = SliceMode::transient;
    float sensitivity = 0.55f;   // 0..1 (UI shows 0–100)
    float minSliceLengthMs = 80.0f; // 20–500
    bool zeroCrossSnap = true;
    double sampleBpm = 120.0;    // Grid mode; seeded from detection, user-editable

    const std::vector<SliceMarker>& getMarkers() const noexcept { return markers; }
    int getNumChops (const SampleData* sample) const noexcept;

    /// Regenerate markers for the current mode/settings. Manual mode keeps
    /// what is there (seeding a single whole-sample chop when empty).
    void regenerate (const SampleData* sample);

    /// Manual edits (any mode). All clamp to neighbours by the minimum slice
    /// length and apply zero-cross snap. Marker 0 s is fixed.
    bool moveMarker (int id, double newTimeSec, const SampleData* sample);
    int addMarker (double timeSec, const SampleData* sample); // returns id or -1
    bool removeMarker (int id);

    void setMarkersFromState (std::vector<SliceMarker> restored);

    SliceMapSnapshot buildSnapshot (const SampleData* sample) const;

    static double snapToZeroCross (const SampleData& sample, double timeSec,
                                   double windowSec = zeroCrossWindowSec);

    /// Grid-mode division for a sensitivity (0: 1/4 … 3: 1/32).
    static int gridDivisionIndex (float sensitivity01) noexcept;
    /// Equal-mode slice count for a sensitivity (2–64).
    static int equalSliceCount (float sensitivity01) noexcept;

private:
    void assignFreshIds();
    double minGapSeconds() const noexcept { return static_cast<double> (minSliceLengthMs) * 0.001; }

    std::vector<SliceMarker> markers; // sorted by time; [0] is always 0 s when non-empty
    int nextId = 1;
};

} // namespace chopline::engine
