#pragma once

#include <juce_core/juce_core.h>

#include "Parameters/ParameterIDs.h"

namespace kickr
{
    /**
        Stage 3 Phase 3.4 — one short, plain-language hover description per parameter
        (user request 2026-08-31: "make short descriptions of knobs and buttons if u
        hover mouse for longer than 3 seconds"). Purely UI sugar: `KickrKnob`/
        `KickrToggle`/`PluginEditor::addCombo` look a parameter's ID up here for its
        `setTooltip()` text instead of the bare `getName()` (which just repeats the
        knob's own on-screen caption). Falls back to `getName()` for anything not
        listed here (there shouldn't be any — all 59 are covered).
    */
    inline const juce::String& paramDescription (juce::StringRef paramID)
    {
        static const std::map<juce::String, juce::String> table {
            // ---- PITCH ------------------------------------------------------------
            { id::fundamental,      "Body pitch — also the MIDI-Pitch mode reference note." },
            { id::pitchStart,       "Pitch envelope start, as a multiple of the fundamental — higher = sharper \"laser\" punch." },
            { id::pitchTime,        "Time for the pitch envelope to fall to the fundamental." },
            { id::pitchCurve,       "Pitch envelope shape: low = smooth sweep, high = sharp exponential snap." },
            // ---- BODY ---------------------------------------------------------------
            { id::bodyLevel,        "Level of the sine body oscillator." },
            { id::bodyDecay,        "Decay time of the body's amplitude envelope." },
            { id::bodyHarmonics,    "Adds harmonic saturation to the body oscillator's own tone." },
            // ---- SUB ------------------------------------------------------------------
            { id::subLevel,         "Level of the independent sub-bass sine layer." },
            { id::subFreq,          "Fixed frequency of the sub layer — doesn't follow the pitch envelope." },
            { id::subDecay,         "Decay time of the sub layer." },
            // ---- CLICK ----------------------------------------------------------------
            { id::clickLevel,       "Level of the synthesized transient click." },
            { id::clickTone,        "Centre frequency of the click's noise burst." },
            { id::clickTime,        "Length of the click's own envelope." },
            { id::clickPitch,       "Frequency of the click's tonal (pitched) component." },
            // ---- TAIL -------------------------------------------------------------------
            { id::tailLevel,        "Level of the sustained tail / rumble layer." },
            { id::tailLength,       "Decay time of the tail layer." },
            { id::tailTone,         "Low-pass brightness of the tail." },
            { id::tailDrive,        "Saturation amount on the tail layer." },
            // ---- NOISE --------------------------------------------------------------------
            { id::noiseLevel,       "Level of the optional noise layer (off by default)." },
            { id::noiseDecay,       "Decay time of the noise layer." },
            { id::noiseTone,        "Tone / filtering of the noise layer." },
            { id::noiseType,        "Noise colour: White, Pink, or Filtered." },
            // ---- SAMPLE (v2) ----------------------------------------------------------------
            { id::synthEnable,      "Turns the synthesized layers on/off." },
            { id::sampleEnable,     "Turns the loaded sample layer on/off." },
            { id::sampleLevel,      "Level of the sample layer." },
            { id::sampleStart,      "Start point of the sample's playback window." },
            { id::sampleEnd,        "End point of the sample's playback window." },
            { id::sampleReverse,    "Plays the sample backwards." },
            { id::sampleTune,       "Sample pitch, in semitones." },
            { id::sampleFine,       "Sample fine-tune, in cents." },
            { id::sampleMidiTrack,  "Tracks incoming MIDI pitch — playing C3 triggers the sample unpitched." },
            { id::sampleAttack,     "Fade-in time of the sample layer." },
            { id::sampleDecay,      "Fade-out time of the sample layer (independent of the file's own length)." },
            { id::sampleHP,         "High-pass filter on the sample layer." },
            { id::sampleLP,         "Low-pass filter on the sample layer." },
            { id::sampleCrush,      "Bitcrush / sample-rate reduction on the sample layer." },
            // ---- TRANSIENT --------------------------------------------------------------------
            { id::transientAttack,  "Sharpens (+) or softens (−) the very onset of the kick." },
            { id::transientSustain, "Boosts (+) or reduces (−) the body right after the onset." },
            // ---- DISTORTION --------------------------------------------------------------------
            { id::drive,            "Master saturation amount." },
            { id::character,        "Morphs the saturation curve from clean to aggressive / digital." },
            { id::driveMix,         "Blends the saturated signal back with the clean one." },
            // ---- TONE --------------------------------------------------------------------------
            { id::low,              "Low-shelf tone control (post-drive)." },
            { id::mid,              "Midrange tone control (post-drive)." },
            { id::high,             "High-shelf tone control (post-drive)." },
            // ---- STEREO --------------------------------------------------------------------------
            { id::bodyWidth,        "Stereo width of the body layer's harmonic content." },
            { id::clickWidth,       "Stereo width of the click layer." },
            { id::outputWidth,      "Overall stereo width of the mix." },
            // ---- OUTPUT --------------------------------------------------------------------------
            { id::output,           "Output gain, applied after the limiter." },
            { id::mix,              "Blends the processed signal with silence (equal-power)." },
            { id::limiter,          "Safety soft-clip limiter on the final output." },
            // ---- GLOBAL --------------------------------------------------------------------------
            { id::oversampling,     "Internal oversampling factor — higher = cleaner distortion, more CPU." },
            // ---- TUNING --------------------------------------------------------------------------
            { id::tuneMode,         "MIDI Pitch (note controls pitch) or Fixed Frequency (always plays at Fundamental)." },
            { id::tune,             "Global tuning offset, in semitones." },
            { id::fineTune,         "Global fine-tune offset, in cents." },
            // ---- VELOCITY --------------------------------------------------------------------------
            { id::velSensitivity,   "How much MIDI velocity affects level and click." },
            // ---- MACROS --------------------------------------------------------------------------
            { id::macroPunch,       "One-knob macro: sharper transient, snappier pitch drop, more click." },
            { id::macroBody,        "One-knob macro: louder / longer body, deeper tune, more sample level." },
            { id::macroCrush,       "One-knob macro: more drive, morphs toward digital / aggressive curves." },
            { id::macroTail,        "One-knob macro: louder, longer, brighter tail." },
        };

        static const juce::String empty;
        const auto it = table.find (juce::String (paramID));
        return it != table.end() ? it->second : empty;
    }
}
