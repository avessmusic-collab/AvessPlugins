// CHOPLINE — Exporter (Phase 5, spec §10 "Export")
// Drag-out MIDI of the current pattern, drag-out AUDIO via a full offline
// engine render, and export-all-chops-as-WAVs. Message/background thread
// only; no GUI module — unit-testable.
//
// MIDI: one format-1 track at 960 PPQ with a tempo meta event. Each pattern
// event becomes a note at (chop base note + chop index) with the lane
// velocity; RATCHETED events export as their audible subdivision (N notes
// with the fixed 0.8× velocity decay), so the file matches what plays.
// Probability < 100 events export unconditionally (the file is the pattern,
// not one roll of it). The per-step pitch lane has no MIDI representation
// (notes ARE the chop mapping) and is not exported — documented.
// Bars are written as 4 quarters (the grid's 4/4 box model; odd-meter caveat
// shared with the pattern bar note in CLAUDE.md).
//
// AUDIO: a private offline Engine instance restores the full state tree and
// renders the active pattern through the complete chain (voices + Beat
// Repeat + master). Determinism: probability/chance are position-hashed and
// grain jitter is trigger-seeded, so render == render == bounce. Spec §8
// offline quality: the render always runs HQ (Eco is ignored offline).
// It renders TWO pattern cycles and returns the SECOND, so tails wrap into
// the loop start and the WAV loops seamlessly (documented).

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include "../engine/Engine.h"
#include "PatternModel.h"

namespace chopline::state
{

/// Builds a MIDI file of one pattern slot. `baseNote` = chop 0's note.
juce::MidiFile buildPatternMidi (const PatternModel& model, int slot,
                                 int baseNote, double bpm);

/// Writes `midi` to `file` (overwrites). Returns false on IO failure.
bool writeMidiFile (const juce::MidiFile& midi, const juce::File& file);

struct OfflineRenderSpec
{
    double sampleRate = 48000.0;
    int blockSize = 512;
    double bpm = 120.0;
    int slot = 0;
    /// Beat Repeat / master / swing / MIDI base for the render — fill from the
    /// live APVTS values. Quality is forced to HQ (eco ignored, spec §8).
    engine::Engine::BlockContext context;
    /// Render two pattern cycles and keep the second (seamless loop). Off =
    /// one cycle from silence.
    bool keepSecondCycle = true;
    /// Optional progress callback (0..1), called per block on the render thread.
    std::function<void (float)> onProgress;
};

/// Renders the pattern offline through a private full engine. Returns an
/// empty buffer and sets `error` when there is nothing to render.
juce::AudioBuffer<float> renderPatternOffline (const juce::ValueTree& fullState,
                                               const OfflineRenderSpec& spec,
                                               juce::String& error);

/// Writes a buffer as a 24-bit WAV. Returns false on IO failure.
bool writeWavFile (const juce::AudioBuffer<float>& buffer, double sampleRate,
                   const juce::File& file);

/// Exports every chop's EFFECTIVE (trimmed) region as "NN <sample>.wav" into
/// `dir` (raw audio, no FX — documented). Returns the number written.
int exportChopWavs (engine::Engine& eng, const juce::File& dir, juce::String& error);

} // namespace chopline::state
