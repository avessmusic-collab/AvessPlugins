#pragma once

#include <array>
#include <memory>
#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters/ParameterIDs.h"

/**
    Table-driven APVTS parameter layout for KickDesigner2.

    45 automatable parameters total (parameter-spec.md LOCKED v1 + architecture.md
    "Parameter Mapping"):
        - 41 juce::AudioParameterFloat
        -  3 juce::AudioParameterChoice  (noiseType, oversampling, tuneMode)
        -  1 juce::AudioParameterBool    (limiter)

    Ranges / defaults / skews are taken verbatim from the locked spec. Skew < 1 on the
    frequency params (fundamental, subFreq, clickTone, clickPitch) expands low-end
    resolution, matching the spec's "log" intent. Bipolar / centre-detent params
    (transientAttack, transientSustain, low, mid, high, tune, fineTune) use a plain
    linear range with default 0 — the centre-detent behaviour is a Stage 3 Knob concern.

    NOTE (output): 0 dB in a -24..+12 range normalises to 24/36 ≈ 0.6667, NOT 0.5.
    The future native Knob's double-click-reset must target ~0.6667 normalised.
*/
namespace kd2
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

    // 41 float parameters, grouped exactly as parameter-spec.md.
    inline constexpr std::array<FloatParamSpec, 41> kFloatParams { {
        // ---- PITCH ----
        { id::fundamental,      "Fundamental",          25.0f,   150.0f,   0.1f,   0.5f,   55.0f,  "Hz" },
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

    inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        for (const auto& p : kFloatParams)
        {
            layout.add (std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID { p.id, 1 },
                juce::String (p.name),
                juce::NormalisableRange<float> (p.minValue, p.maxValue, p.interval, p.skew),
                p.defaultValue,
                juce::AudioParameterFloatAttributes().withLabel (juce::String (p.unitLabel))));
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
            1)); // provisional 2x — confirmed at repo Stage 17 profiling

        layout.add (std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { id::tuneMode, 1 },
            "Tune Mode",
            juce::StringArray { "MIDI Pitch", "Fixed Frequency" },
            0));

        // ---- 1 Bool parameter ----
        layout.add (std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { id::limiter, 1 },
            "Limiter",
            true));

        return layout;
    }
}
