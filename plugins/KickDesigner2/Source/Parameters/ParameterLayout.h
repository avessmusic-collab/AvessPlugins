#pragma once

#include <array>
#include <memory>
#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters/ParameterIDs.h"

/**
    Table-driven APVTS parameter layout for KickDesigner2.

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

    // 51 float parameters, grouped exactly as parameter-spec.md (41 v1 core + 10 v2 SAMPLE).
    inline constexpr std::array<FloatParamSpec, 51> kFloatParams { {
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

        // ---- SAMPLE (v2) ----
        { id::sampleLevel,      "Sample Level",          0.0f,     1.0f,   0.001f, 1.0f,    0.7f,  ""   },
        { id::sampleStart,      "Sample Start",          0.0f,     1.0f,   0.0005f,1.0f,    0.0f,  ""   },
        { id::sampleEnd,        "Sample End",            0.0f,     1.0f,   0.0005f,1.0f,    1.0f,  ""   },
        { id::sampleTune,       "Sample Tune",         -24.0f,    24.0f,   1.0f,   1.0f,    0.0f,  "st" },
        { id::sampleFine,       "Sample Fine",        -100.0f,   100.0f,   1.0f,   1.0f,    0.0f,  "ct" },
        { id::sampleAttack,     "Sample Attack",         0.0f,   200.0f,   0.1f,   0.35f,   0.0f,  "ms" },
        { id::sampleDecay,      "Sample Decay",         20.0f,  2000.0f,   1.0f,   0.4f,  800.0f,  "ms" },
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
