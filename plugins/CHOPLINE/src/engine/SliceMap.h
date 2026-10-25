// CHOPLINE — SliceMap (spec §4, §6)
// Slice markers, per-chop playback settings and per-chop playback bounds.
//
// The MODEL (SliceModel) lives on the message thread and stores markers in
// SECONDS (so a session-rate change never moves a slice). The four slicing
// modes all just generate markers; every mode's markers can then be edited
// manually (drag / add / delete). The AUDIO side sees an immutable
// SliceMapSnapshot (sample-frame chop bounds + per-chop playback settings
// converted to frames) published through SnapshotExchange.
//
// Phase 3: every marker carries the full per-chop settings of the chop that
// STARTS at it (locked v17 `scope: per_chop` — model state, NOT APVTS):
// sample-editor trims/fades/loop, playback mode, warp, pitch, release, sends,
// unison, the AHDSR amp envelope and the choke group. Settings are copied by
// value into the snapshot, so editing them never glitches ringing voices
// (they finish on the settings they were triggered with).
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

// ---- per-chop playback settings (locked v17, spec §6) ---------------------------

enum class PlaybackMode : int
{
    oneShot = 0, // start → end, ignores event length / note-off
    gate,        // plays for the event span / while the note is held, then releases
    loop,        // loops loopStart..loopEnd (crossfaded) for the span / note hold
    fitStep      // time-stretched to exactly fill the event span, pitch preserved
};

enum class WarpMode : int
{
    varispeed = 0, // speed & pitch together (±24 st)
    stretch,       // Signalsmith Stretch: independent pitch, formant preserve
    granular,      // Phase 5 (surfaced disabled in the v17 UI)
    freeze         // Phase 5 (surfaced disabled in the v17 UI)
};

/// Settings of one chop (the chop starting at a marker). Model units:
/// seconds / ms / normalised — converted to frames in the snapshot.
struct ChopSettings
{
    // Sample editor (v17 chop_sample_editor per_chop_model_fields).
    double startOffsetSec = 0.0; // trim of the chop start vs. its marker (signed)
    double endOffsetSec = 0.0;   // trim of the chop end vs. the next marker (signed)
    float fadeInMs = 2.0f;       // equal-power declick fades, 0–20 ms
    float fadeOutMs = 2.0f;
    double loopStart = 0.25;     // normalised 0..1 of the trimmed chop
    double loopEnd = 0.75;       // loopStart <= loopEnd - 0.05
    float loopXfadeMs = 10.0f;   // 0–200, clamped to half the loop at build time

    // Playback (v17 overlay / FX CHOP column).
    PlaybackMode mode = PlaybackMode::gate; // v17 default Gate
    WarpMode warp = WarpMode::varispeed;
    bool reverse = false;
    int pitchSemitones = 0;   // ±24, step 1
    float releaseMs = 80.0f;  // 5–500 — also the AHDSR release stage
    float delaySend = 0.0f;   // 0–100 % (stored now; audible with the Phase 4 delay)
    float verbSend = 0.0f;    // 0–100 % (stored now; audible with the Phase 4 reverb)
    int unison = 1;           // 1–4 voices (fixed cents + stereo spread)

    // AHDSR amp envelope (v17 ENVELOPE card; release is releaseMs above).
    float attackMs = 2.0f;    // 0–500
    float holdMs = 0.0f;      // 0–500
    float decayMs = 160.0f;   // 5–1000
    float sustainPct = 100.0f; // 0–100

    // Engine-only (no v17 UI yet).
    int chokeGroup = 1;         // 0 = off; default: ALL chops in group 1 (spec §6)
    bool formantPreserve = true; // Stretch warp formant preservation

    /// Returns a range-clamped copy (offsets are clamped against neighbours
    /// in SliceModel::buildSnapshot, not here).
    static ChopSettings clamped (ChopSettings s) noexcept;
};

struct SliceMarker
{
    int id = 0;          // stable while the marker lives
    double timeSec = 0;  // position in the sample
    ChopSettings settings; // settings of the chop STARTING at this marker
};

/// Immutable audio-thread view: chop bounds in sample frames at session rate
/// plus the per-chop playback settings (copied by value into voices).
struct SliceMapSnapshot
{
    static constexpr int maxChops = 64;

    struct Chop
    {
        juce::int64 start = 0, end = 0; // EFFECTIVE [start, end) incl. trim offsets
        int fadeInSamples = 0, fadeOutSamples = 0;

        PlaybackMode mode = PlaybackMode::gate;
        WarpMode warp = WarpMode::varispeed;
        bool reverse = false;
        bool formantPreserve = true;
        float pitchSemitones = 0.0f;
        int unison = 1;
        int chokeGroup = 1;
        float delaySend = 0.0f, verbSend = 0.0f; // 0..1 (Phase 4 master sends)

        juce::int64 loopStart = 0, loopEnd = 0;  // absolute frames inside [start, end)
        int loopXfadeSamples = 0;

        int attackSamples = 0, holdSamples = 0, decaySamples = 0, releaseSamples = 1;
        float sustainLevel = 1.0f; // 0..1
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

    // ---- per-chop settings (Phase 3) ----------------------------------------
    /// Settings of chop `index` (marker order). Returns defaults when out of
    /// range.
    ChopSettings getChopSettings (int index) const noexcept;
    /// Replaces chop `index` settings (range-clamped). Returns true if valid.
    bool setChopSettings (int index, const ChopSettings& s) noexcept;

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
