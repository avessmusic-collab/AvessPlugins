#pragma once

/**
    Single source of truth for every KICKR APVTS parameter ID string.

    71 automatable parameters: 60 Float, 5 Choice (noiseType, oversampling, tuneMode,
    filterType, morphMode), 6 Bool (limiter, filterOn, synthEnable, sampleEnable,
    sampleReverse, sampleMidiTrack).
    `morph` added 2026-09-01 (body oscillator waveform morph — not in the original v2
    parameter-spec.md; see plugins/KICKR/NOTES.md for the addition). `morphMode` added
    2026-09-02 (Serum-Warp-style mode selector for `morph` — see NOTES.md).
    Randomize / Mutate / Save / sample-bank-browse / sample-drag-drop are UI actions,
    NOT parameters. Sample selection is the state property `currentSampleName`.

    Contract: parameter-spec.md (LOCKED v2 — v1 core 45 + v2 SAMPLE group 14) +
    architecture.md "Parameter Mapping". Do NOT rename / add / remove without a new
    version of parameter-spec.md.
*/
namespace kickr::id
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
    inline constexpr auto morph            = "morph";   // 2026-09-01: body osc waveform morph amount (0..1)
    inline constexpr auto morphMode        = "morphMode";   // 2026-09-02: Choice — which warp mode `morph` drives (8 modes, see BodyOscillator::Mode)

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
    inline constexpr auto limiter          = "limiter";
    // 2026-09-02 (user request): Color-Limiter control set (Ableton Color Limiter layout).
    inline constexpr auto limLoudness      = "limLoudness";      // 0..+24 dB
    inline constexpr auto limCeiling       = "limCeiling";       // -24..0 dB
    inline constexpr auto limLookahead     = "limLookahead";     // 0.1..10 ms
    inline constexpr auto limRelease       = "limRelease";       // 1..1000 ms
    inline constexpr auto limSaturation    = "limSaturation";    // 0..1
    inline constexpr auto limColor         = "limColor";         // 0..1
    // 2026-09-02 (user request): master filter (FILTER page of the scope).
    inline constexpr auto filterOn         = "filterOn";         // Bool, default off
    inline constexpr auto filterType       = "filterType";       // Choice: Low Pass / High Pass
    inline constexpr auto filterFreq       = "filterFreq";       // 20..20000 Hz
    inline constexpr auto filterRes        = "filterRes";        // 0..1    // Bool

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
