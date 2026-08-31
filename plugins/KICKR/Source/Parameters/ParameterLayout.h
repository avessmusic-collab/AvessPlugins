#pragma once

#include <array>
#include <cmath>
#include <memory>
#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters/ParameterIDs.h"

/**
    Table-driven APVTS parameter layout for KICKR.

    59 automatable parameters total (parameter-spec.md LOCKED v2 + architecture.md
    "Parameter Mapping"):
        - 51 juce::AudioParameterFloat   (41 v1 core + 10 v2 SAMPLE)
        -  3 juce::AudioParameterChoice  (noiseType, oversampling, tuneMode)
        -  5 juce::AudioParameterBool    (limiter, synthEnable, sampleEnable,
                                          sampleReverse, sampleMidiTrack)

    Ranges / defaults / skews are taken verbatim from the locked spec. Skew < 1 on the
    frequency params (fundamental, subFreq, clickTone, clickPitch, sampleHP, sampleLP)
    expands low-end resolution, matching the spec's "log" intent. Bipolar / centre-detent
    params (transientAttack, transientSustain, low, mid, high, tune, fineTune, sampleTune,
    sampleFine) use a plain linear range with default 0 — the centre-detent behaviour is
    a Stage 3 Knob concern.

    SAMPLE (v2): the sample layer is the user's own recorded kick from the managed bank.
    Sample *selection* is NOT a parameter — it is the ValueTree property currentSampleName
    (see PluginProcessor state). sampleEnable defaults off so v1 patches sound identical.

    NOTE (output): 0 dB in a -24..+12 range normalises to 24/36 ≈ 0.6667, NOT 0.5.
    The future native Knob's double-click-reset must target ~0.6667 normalised.
*/
namespace kickr
{
    struct FloatParamSpec
    {
        const char* id;
        const char* name;
        float       minValue;
        float       maxValue;
        float       interval;
        float       skew;          // 1.0f = linear
        float       defaultValue;
        const char* unitLabel;     // "" when unitless
    };

    // 51 float parameters, grouped exactly as parameter-spec.md (41 v1 core + 10 v2 SAMPLE).
    inline constexpr std::array<FloatParamSpec, 51> kFloatParams { {
        // ---- PITCH ----
        { id::fundamental,      "Fundamental",          25.0f,   150.0f,   0.1f,   0.5f,   55.0f,  ""   }, // shown as a note (A1); see createParameterLayout special case
        { id::pitchStart,       "Pitch Start",           1.0f,    10.0f,   0.01f,  0.6f,    4.0f,  "x"  },
        { id::pitchTime,        "Pitch Time",            5.0f,   500.0f,   0.1f,   0.3f,   50.0f,  "ms" },
        { id::pitchCurve,       "Pitch Curve",           0.0f,     1.0f,   0.001f, 1.0f,    0.7f,  ""   },

        // ---- BODY ----
        { id::bodyLevel,        "Body Level",            0.0f,     1.0f,   0.001f, 1.0f,    1.0f,  ""   },
        { id::bodyDecay,        "Body Decay",           20.0f,  2000.0f,   1.0f,   0.4f,  400.0f,  "ms" },
        { id::bodyHarmonics,    "Body Harmonics",        0.0f,     1.0f,   0.001f, 1.0f,    0.15f, ""   },

        // ---- SUB ----
        { id::subLevel,         "Sub Level",             0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },
        { id::subFreq,          "Sub Frequency",        25.0f,    80.0f,   0.1f,   0.57f,  40.0f,  "Hz" },
        { id::subDecay,         "Sub Decay",            20.0f,  2000.0f,   1.0f,   0.35f, 300.0f,  "ms" },

        // ---- CLICK ----
        { id::clickLevel,       "Click Level",           0.0f,     1.0f,   0.001f, 1.0f,    0.4f,  ""   },
        { id::clickTone,        "Click Tone",         1000.0f, 15000.0f,   1.0f,   0.45f, 4000.0f, "Hz" },
        { id::clickTime,        "Click Time",            0.1f,    50.0f,   0.01f,  0.25f,   3.0f,  "ms" },
        { id::clickPitch,       "Click Pitch",        1000.0f, 15000.0f,   1.0f,   0.55f, 5000.0f, "Hz" },

        // ---- TAIL ----
        { id::tailLevel,        "Tail Level",            0.0f,     1.0f,   0.001f, 1.0f,    0.3f,  ""   },
        { id::tailLength,       "Tail Length",          20.0f,  2000.0f,   1.0f,   0.3f,  200.0f,  "ms" },
        { id::tailTone,         "Tail Tone",             0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },
        { id::tailDrive,        "Tail Drive",            0.0f,     1.0f,   0.001f, 1.0f,    0.2f,  ""   },

        // ---- NOISE ----
        { id::noiseLevel,       "Noise Level",           0.0f,     1.0f,   0.001f, 1.0f,    0.0f,  ""   },
        { id::noiseDecay,       "Noise Decay",          20.0f,   500.0f,   1.0f,   0.28f,  60.0f,  "ms" },
        { id::noiseTone,        "Noise Tone",            0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },

        // ---- SAMPLE (v2) ----
        { id::sampleLevel,      "Sample Level",          0.0f,     1.0f,   0.001f, 1.0f,    0.7f,  ""   },
        { id::sampleStart,      "Sample Start",          0.0f,     1.0f,   0.0005f,1.0f,    0.0f,  ""   },
        { id::sampleEnd,        "Sample End",            0.0f,     1.0f,   0.0005f,1.0f,    1.0f,  ""   },
        { id::sampleTune,       "Sample Tune",         -24.0f,    24.0f,   1.0f,   1.0f,    0.0f,  "st" },
        { id::sampleFine,       "Sample Fine",        -100.0f,   100.0f,   1.0f,   1.0f,    0.0f,  "ct" },
        { id::sampleAttack,     "Sample Attack",         0.0f,   200.0f,   0.1f,   0.35f,   0.0f,  "ms" },
        // 2026-08-31 (user bug report — "sample doesn't play whole, especially reversed"):
        // sampleDecay reaches its -60 dB point AT its own value in ms and is fully silent
        // (SamplePlayer::kFloorGain, -100 dB) at ~1.667x that — so the OLD 800 ms default /
        // 2000 ms max silenced ANY sample past ~1.3 s / 3.3 s respectively, well short of
        // the 5 s import cap (kMaxSampleSeconds). Reverse made this obvious: the loud
        // transient sits at the temporal END of reversed playback and was routinely
        // decayed to silence before ever being heard. Resolves the parameter-spec's own
        // long-open "sampleDecay max = play-to-end vs hard cap" question in favour of
        // covering the full import range, with a default generous enough that a typical
        // one-shot/riser plays out by default (no factory preset touches sampleDecay —
        // all 17 are synth-only — so this has zero effect on any of them).
        { id::sampleDecay,      "Sample Decay",         20.0f,  5500.0f,   1.0f,   0.4f, 2200.0f,  "ms" },
        { id::sampleHP,         "Sample HP",            20.0f,  2000.0f,   1.0f,   0.4f,   20.0f,  "Hz" }, // 20 = Off
        { id::sampleLP,         "Sample LP",           200.0f, 20000.0f,   1.0f,   0.4f, 20000.0f, "Hz" }, // 20000 = Off
        { id::sampleCrush,      "Sample Crush",          0.0f,     1.0f,   0.001f, 1.0f,    0.0f,  ""   },

        // ---- TRANSIENT (bipolar, centre detent) ----
        { id::transientAttack,  "Transient Attack",     -1.0f,     1.0f,   0.001f, 1.0f,    0.0f,  ""   },
        { id::transientSustain, "Transient Sustain",    -1.0f,     1.0f,   0.001f, 1.0f,    0.0f,  ""   },

        // ---- DISTORTION ----
        { id::drive,            "Drive",                 0.0f,     1.0f,   0.001f, 1.0f,    0.3f,  ""   },
        { id::character,        "Character",             0.0f,     1.0f,   0.001f, 1.0f,    0.0f,  ""   },
        { id::driveMix,         "Drive Mix",             0.0f,     1.0f,   0.001f, 1.0f,    1.0f,  ""   },

        // ---- TONE (bipolar dB, centre detent) ----
        { id::low,              "Low",                 -12.0f,    12.0f,   0.1f,   1.0f,    0.0f,  "dB" },
        { id::mid,              "Mid",                 -12.0f,    12.0f,   0.1f,   1.0f,    0.0f,  "dB" },
        { id::high,             "High",                -12.0f,    12.0f,   0.1f,   1.0f,    0.0f,  "dB" },

        // ---- STEREO ----
        { id::bodyWidth,        "Body Width",            0.0f,     1.0f,   0.001f, 1.0f,    0.0f,  ""   },
        { id::clickWidth,       "Click Width",           0.0f,     1.0f,   0.001f, 1.0f,    0.3f,  ""   },
        { id::outputWidth,      "Output Width",          0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },

        // ---- OUTPUT ----
        { id::output,           "Output",              -24.0f,    12.0f,   0.1f,   1.0f,    0.0f,  "dB" }, // 0 dB -> ~0.6667 normalised
        { id::mix,              "Mix",                   0.0f,     1.0f,   0.001f, 1.0f,    1.0f,  ""   },

        // ---- TUNING ----
        { id::tune,             "Tune",                -24.0f,    24.0f,   1.0f,   1.0f,    0.0f,  "st" },
        { id::fineTune,         "Fine Tune",          -100.0f,   100.0f,   1.0f,   1.0f,    0.0f,  "ct" },

        // ---- VELOCITY ----
        { id::velSensitivity,   "Velocity Sensitivity",  0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },

        // ---- MACROS (0.5 = neutral) ----
        { id::macroPunch,       "Punch",                 0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },
        { id::macroBody,        "Body",                  0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },
        { id::macroCrush,       "Crush",                 0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },
        { id::macroTail,        "Tail",                  0.0f,     1.0f,   0.001f, 1.0f,    0.5f,  ""   },
    } };

    // Nearest chromatic note name for a frequency in Hz (440 Hz = A4, 55 Hz = A1).
    inline juce::String hzToNoteName (float hz)
    {
        if (hz <= 0.0f) return "-";
        static const char* names[] { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        const int midi = (int) std::lround (69.0 + 12.0 * std::log2 ((double) hz / 440.0));
        return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }

    // Stage 3 Phase 3.4 (user request 2026-08-31): `character` morphs continuously through
    // 7 named curves (Waveshaper.h, LOCKED order) — show which one(s), not a bare 0..1
    // number. `seg = floor(character*6)` / `t` matches the DSP's own crossface math exactly.
    inline juce::String characterCurveName (float character01) noexcept
    {
        // Short forms — this reads out in a small knob's one-line value box, so keep it
        // tight (the full names live in the CHARACTR tooltip instead).
        static const char* const names[7] =
            { "Tanh", "Cubic", "Asym", "Soft", "Hard", "Fold", "Crush" };

        const float c   = juce::jlimit (0.0f, 1.0f, character01) * 6.0f;
        const int   seg = juce::jlimit (0, 5, (int) std::floor (c));
        const float t   = c - (float) seg;

        if (t < 0.08f) return names[seg];
        if (t > 0.92f) return names[seg + 1];
        return juce::String (names[seg]) + "/" + names[seg + 1];
    }

    // Frequency (Hz) for a chromatic note name like "A1" / "C#2".
    inline float noteNameToHz (const juce::String& text)
    {
        auto s = text.trim().toUpperCase();
        if (s.isEmpty()) return 55.0f;
        static const char* names[] { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        int idx = -1, pos = 1;
        if (s.length() >= 2 && s[1] == '#') { pos = 2; }
        const auto pc = s.substring (0, pos);
        for (int i = 0; i < 12; ++i) if (pc == names[i]) idx = i;
        if (idx < 0) return text.getFloatValue() > 0.0f ? text.getFloatValue() : 55.0f; // fall back to a Hz string
        const int octave = s.substring (pos).getIntValue();
        const int midi = (octave + 1) * 12 + idx;
        return (float) (440.0 * std::pow (2.0, (midi - 69) / 12.0));
    }

    inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        for (const auto& p : kFloatParams)
        {
            auto attrs = juce::AudioParameterFloatAttributes().withLabel (juce::String (p.unitLabel));

            // Fundamental is dialled as a musical note, not raw Hz (55 Hz shows as "A1").
            if (juce::String (p.id) == id::fundamental)
                attrs = attrs
                    .withLabel ({})
                    .withStringFromValueFunction ([] (float v, int) { return hzToNoteName (v); })
                    .withValueFromStringFunction ([] (const juce::String& t) { return noteNameToHz (t); });

            // Character shows which of the 7 morph curves it's on/between, not a bare number.
            if (juce::String (p.id) == id::character)
                attrs = attrs
                    .withLabel ({})
                    .withStringFromValueFunction ([] (float v, int) { return characterCurveName (v); });

            layout.add (std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID { p.id, 1 },
                juce::String (p.name),
                juce::NormalisableRange<float> (p.minValue, p.maxValue, p.interval, p.skew),
                p.defaultValue,
                attrs));
        }

        // ---- 3 Choice parameters ----
        layout.add (std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { id::noiseType, 1 },
            "Noise Type",
            juce::StringArray { "White", "Pink", "Filtered" },
            0));

        layout.add (std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { id::oversampling, 1 },
            "Oversampling",
            juce::StringArray { "1x", "2x", "4x", "8x" },
            1)); // 2x — LOCKED 2026-08-31 (repo Stage 17 profiling, RunTests.cpp "[Stage 17]"):
                 // 18-36x real-time at 44.1/48 kHz on dev hardware (monophonic engine, at
                 // most 2 voices during a 3 ms retrigger crossfade — CPU cost is low at
                 // every factor); 2x already pushes the waveshaper/sampleCrush aliasing
                 // well above the kick's own energy. 4x/8x stay available per-patch.

        layout.add (std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { id::tuneMode, 1 },
            "Tune Mode",
            juce::StringArray { "MIDI Pitch", "Fixed Frequency" },
            0));

        // ---- 5 Bool parameters ----
        layout.add (std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { id::limiter, 1 }, "Limiter", true));

        // SAMPLE (v2). Defaults chosen so v1 behaviour is preserved:
        // synthEnable on, sampleEnable off  ->  identical to the v1 synth-only engine.
        layout.add (std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { id::synthEnable, 1 }, "Synth Enable", true));
        layout.add (std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { id::sampleEnable, 1 }, "Sample Enable", false));
        layout.add (std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { id::sampleReverse, 1 }, "Sample Reverse", false));
        layout.add (std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { id::sampleMidiTrack, 1 }, "Sample MIDI Track", true));

        return layout;
    }
}
