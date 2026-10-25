// CHOPLINE — Parameters (spec §3, §10)
// APVTS layout, parameter IDs and ranges. Stable IDs with a version number
// from day one; schema migrations are unit-tested against every previous
// version (spec §10).
//
// Phase 1–2: midiBaseNote + swing (slicing controls are slice-map STATE, not
// parameters — locked v17 design, scope: slice_map).
// Phase 4: the FULL Beat Repeat parameter set (spec §7 — all automatable even
// where the locked v17 UI surfaces only HOLD/AUTO/EVERY/GRID/CHANCE/DECAY;
// the rest ship with the v17 `not_surfaced_in_mockup` defaults for automation
// and a later advanced drawer) and the master chain (filter / distortion /
// width / output+limiter / delay / reverb with per-section bypass).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace chopline::params
{

/// Bumped whenever the state schema changes; migrations keyed off this.
inline constexpr int kSchemaVersion = 1;

/// Parameter ID version suffix (juce::ParameterID version hint).
inline constexpr int kParameterVersion = 1;

namespace id
{
    inline constexpr auto midiBaseNote = "midiBaseNote"; // choice: C0 C1 C2 C3
    inline constexpr auto swing = "swing";               // float: 0–75 % (Phase 2)

    // ---- Beat Repeat (Phase 4, spec §7; ids = locked v17 ids) ----------------
    inline constexpr auto brMode = "br_mode";             // choice: Auto Manual Off
    inline constexpr auto brHold = "br_hold";             // bool: manual latch (HOLD)
    inline constexpr auto brInterval = "br_interval";     // choice: 1/4 … 4 bars
    inline constexpr auto brOffset = "br_offset";         // choice: 0–15 (interval/16)
    inline constexpr auto brChance = "br_chance";         // float: 0–100 %
    inline constexpr auto brGrid = "br_grid";             // choice: 1/2 … 1/32T
    inline constexpr auto brGate = "br_gate";             // choice: 1/16 … 4 bars
    inline constexpr auto brDecay = "br_decay";           // float: 0–100 %/repeat
    inline constexpr auto brPitchDecay = "br_pdecay";     // float: 0–12 st, 0.5 steps
    inline constexpr auto brAccel = "br_accel";           // float: 0–100 %
    inline constexpr auto brReverse = "br_reverse";       // choice: Off Alternate All
    inline constexpr auto brFilterMode = "br_filter_mode"; // choice: Off LP HP
    inline constexpr auto brSweep = "br_sweep";           // float: 0–100 %
    inline constexpr auto brMix = "br_mix";               // choice: Mix Insert Gate
    inline constexpr auto brFreeze = "br_freeze";         // bool
    inline constexpr auto brRndGrid = "br_rnd_grid";      // bool
    inline constexpr auto brMidiBase = "br_midi_base";    // choice: C2 C3 C4 C5

    // ---- master chain (Phase 4; ids = locked v17 ids) --------------------------
    inline constexpr auto masterFilterOn = "master_filter_on";
    inline constexpr auto masterFilterType = "master_filter_type"; // LP BP HP
    inline constexpr auto masterFilterCutoff = "master_filter_cutoff"; // 20–20k Hz log
    inline constexpr auto masterFilterReso = "master_filter_reso";     // 0–100 %

    inline constexpr auto masterDistOn = "master_dist_on";
    inline constexpr auto masterDistType = "master_dist_type"; // 6 types
    inline constexpr auto masterDistDrive = "master_dist_drive"; // 0–100 %
    inline constexpr auto masterDistMix = "master_dist_mix";     // 0–100 %
    inline constexpr auto masterDistAutoGain = "master_dist_autogain";

    inline constexpr auto masterWidthOn = "master_width_on";
    inline constexpr auto masterWidth = "master_width"; // 0–200 %
    inline constexpr auto masterMono = "master_mono";   // mono-below 20–300 Hz

    inline constexpr auto masterLimiterOn = "master_limiter_on"; // OUTPUT section
    inline constexpr auto masterOutput = "master_output";   // −24…+12 dB
    inline constexpr auto masterCeiling = "master_ceiling"; // −12…0 dB

    inline constexpr auto masterDelayOn = "master_delay_on";
    inline constexpr auto masterDelayTime = "master_delay_time"; // tempo-synced
    inline constexpr auto masterDelayFb = "master_delay_fb";     // 0–95 %
    inline constexpr auto masterDelayRtn = "master_delay_rtn";   // 0–100 %

    inline constexpr auto masterVerbOn = "master_verb_on";
    inline constexpr auto masterVerbDecay = "master_verb_decay"; // 0–100 %
    inline constexpr auto masterVerbDamp = "master_verb_damp";   // 0–100 %
    inline constexpr auto masterVerbRtn = "master_verb_rtn";     // 0–100 %
}

/// Choice index (0..3) → MIDI note number (C0=24 … C3=60, Ableton naming).
inline constexpr int baseNoteForChoice (int choiceIndex) noexcept
{
    return 24 + 12 * (choiceIndex < 0 ? 0 : (choiceIndex > 3 ? 3 : choiceIndex));
}

/// Beat Repeat performance-note base: choice 0..3 → C2/C3/C4/C5 = 48/60/72/84
/// (Ableton naming). Spec §7: C3 = 1/4, C#3 = 1/8, … (8 notes, base + 0..7).
inline constexpr int brBaseNoteForChoice (int choiceIndex) noexcept
{
    return 48 + 12 * (choiceIndex < 0 ? 0 : (choiceIndex > 3 ? 3 : choiceIndex));
}

/// Tempo-synced delay time options (v17 master_delay_time) in BEATS (quarters):
/// 1/16, 1/8, 1/8., 1/4, 1/4., 1/2, 1/8T, 1/4T.
inline constexpr double delayTimeBeats (int choiceIndex) noexcept
{
    constexpr double table[8] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 1.0 / 3.0, 2.0 / 3.0 };
    return table[choiceIndex < 0 ? 0 : (choiceIndex > 7 ? 7 : choiceIndex)];
}

/// Builds the full APVTS parameter layout.
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace chopline::params
