#pragma once

/**
    Single source of truth for every KickDesigner2 APVTS parameter ID string.

    59 automatable parameters: 51 Float, 3 Choice (noiseType, oversampling, tuneMode),
    5 Bool (limiter, synthEnable, sampleEnable, sampleReverse, sampleMidiTrack).
    Randomize / Mutate / Save / sample-bank-browse / sample-drag-drop are UI actions,
    NOT parameters. Sample selection is the state property `currentSampleName`.

    Contract: parameter-spec.md (LOCKED v2 — v1 core 45 + v2 SAMPLE group 14) +
    architecture.md "Parameter Mapping". Do NOT rename / add / remove without a new
    version of parameter-spec.md.
*/
namespace kd2::id
{
    // ---- PITCH ---------------------------------------------------------------
    inline constexpr auto fundamental      = "fundamental";
    inline constexpr auto pitchStart       = "pitchStart";
    inline constexpr auto pitchTime        = "pitchTime";
    inline constexpr auto pitchCurve       = "pitchCurve";

    // ---- BODY ---------------------------------------------------------------
    inline constexpr auto bodyLevel        = "bodyLevel";
    inline constexpr auto bodyDecay        = "bodyDecay";
    inline constexpr auto bodyHarmonics    = "bodyHarmonics";

    // ---- SUB --------------------------------------------------------------- -
    inline constexpr auto subLevel         = "subLevel";
    inline constexpr auto subFreq          = "subFreq";
    inline constexpr auto subDecay         = "subDecay";

    // ---- CLICK ------------------------------------------------------------- -
    inline constexpr auto clickLevel       = "clickLevel";
    inline constexpr auto clickTone        = "clickTone";
    inline constexpr auto clickTime        = "clickTime";
    inline constexpr auto clickPitch       = "clickPitch";

    // ---- TAIL ------------------------------------------------------------- --
    inline constexpr auto tailLevel        = "tailLevel";
    inline constexpr auto tailLength       = "tailLength";
    inline constexpr auto tailTone         = "tailTone";
    inline constexpr auto tailDrive        = "tailDrive";

    // ---- NOISE ------------------------------------------------------------- -
    inline constexpr auto noiseLevel       = "noiseLevel";
    inline constexpr auto noiseDecay       = "noiseDecay";
    inline constexpr auto noiseTone        = "noiseTone";
    inline constexpr auto noiseType        = "noiseType";   // Choice

    // ---- SAMPLE (v2) ----------------------------------------------------------
    inline constexpr auto synthEnable      = "synthEnable";      // Bool (default true)
    inline constexpr auto sampleEnable     = "sampleEnable";     // Bool (default false)
    inline constexpr auto sampleLevel      = "sampleLevel";
    inline constexpr auto sampleStart      = "sampleStart";
    inline constexpr auto sampleEnd        = "sampleEnd";
    inline constexpr auto sampleReverse    = "sampleReverse";    // Bool (default false)
    inline constexpr auto sampleTune       = "sampleTune";
    inline constexpr auto sampleFine       = "sampleFine";
    inline constexpr auto sampleMidiTrack  = "sampleMidiTrack";  // Bool (default true)
    inline constexpr auto sampleAttack     = "sampleAttack";
    inline constexpr auto sampleDecay      = "sampleDecay";
    inline constexpr auto sampleHP         = "sampleHP";
    inline constexpr auto sampleLP         = "sampleLP";
    inline constexpr auto sampleCrush      = "sampleCrush";

    // ---- TRANSIENT ------------------------------------------------------- ---
    inline constexpr auto transientAttack  = "transientAttack";
    inline constexpr auto transientSustain = "transientSustain";

    // ---- DISTORTION ----------------------------------------------------- ----
    inline constexpr auto drive            = "drive";
    inline constexpr auto character        = "character";
    inline constexpr auto driveMix         = "driveMix";

    // ---- TONE ---------------------------------------------------------- -----
    inline constexpr auto low              = "low";
    inline constexpr auto mid              = "mid";
    inline constexpr auto high             = "high";

    // ---- STEREO ------------------------------------------------------- ------
    inline constexpr auto bodyWidth        = "bodyWidth";
    inline constexpr auto clickWidth       = "clickWidth";
    inline constexpr auto outputWidth      = "outputWidth";

    // ---- OUTPUT ----------------------------------------------------- -------
    inline constexpr auto output           = "output";
    inline constexpr auto mix              = "mix";
    inline constexpr auto limiter          = "limiter";    // Bool

    // ---- GLOBAL --------------------------------------------------- --------
    inline constexpr auto oversampling     = "oversampling"; // Choice

    // ---- TUNING ------------------------------------------------- ---------
    inline constexpr auto tuneMode         = "tuneMode";   // Choice
    inline constexpr auto tune             = "tune";
    inline constexpr auto fineTune         = "fineTune";

    // ---- VELOCITY --------------------------------------------- ----------
    inline constexpr auto velSensitivity   = "velSensitivity";

    // ---- MACROS ------------------------------------------- --------------
    inline constexpr auto macroPunch       = "macroPunch";
    inline constexpr auto macroBody        = "macroBody";
    inline constexpr auto macroCrush       = "macroCrush";
    inline constexpr auto macroTail        = "macroTail";
}
