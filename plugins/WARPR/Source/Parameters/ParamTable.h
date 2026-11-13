#pragma once
/*
  ==============================================================================
   ParamTable.h - THE single source of truth for WARPR's 135 parameters.

   Generated from .ideas/parameter-spec.md (LOCKED).  Order is the exact APVTS layout order:
   Sample, Granular, Warp, Shaper, Envelope, Filter, LFO 1..10, Global.

   Entry macros (a consumer passes four macro names to WARPR_PARAM_TABLE):
     XF (id, name, min, max, step, skewCentre, default, unit)   Float  (step 0 = continuous,
                                                                         skewCentre 0 = linear)
     XI (id, name, min, max, default, unit)                     Int
     XB (id, name, default)                                     Bool
     XC (id, name, defaultIndex, choiceList, automatable)       Choice (list in warpr::choices)

   Consumers: ParamId enum + id strings + metadata (this file), APVTS layout
   (ParameterLayout.cpp), ParamSnapshot (ParamSnapshot.h).  The editor's relay macro tables
   (WARPR_*_PARAMS in PluginEditor.h) are checked against this table by WARPR_Tests.

   This header has no JUCE dependency so the console tests / scripts can include it freely.
  ==============================================================================
*/

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

//==============================================================================
// Choice lists (append-only after release: never remove or re-index an entry; keep dead
// entries instead).  Non-ASCII glyphs are written as UTF-8 byte escapes (each escape is
// followed by a separate string literal so a hex escape never swallows a digit) and are
// decoded with juce::String::fromUTF8 in the layout builder.
//==============================================================================
namespace warpr::choices
{
    inline constexpr const char* loopMode[]          = { "Forward Loop", "Ping-Pong", "One-Shot" };
    inline constexpr const char* direction[]         = { "Forward", "Reverse" };
    inline constexpr const char* playMode[]          = { "Classic", "Granular" };
    inline constexpr const char* grainMode[]         = { "Classic", "Loop", "Cloud" };
    inline constexpr const char* modOscSource[]      = { "Sine", "Triangle", "Saw", "Square", "Sample" };
    inline constexpr const char* oversampling[]      = { "1x", "2x", "4x", "8x" };
    inline constexpr const char* shaperCurveSelect[] = { "A", "B" };
    inline constexpr const char* shaperPreset[]      = { "Soft Sat", "Hard Clip", "Tube", "Diode", "Fold",
                                                         "Sine", "Triangle", "Expo", "Custom" };
    inline constexpr const char* shaperSymmetry[]    = { "Symmetric", "Asymmetric" };
    inline constexpr const char* lfoMode[]           = { "Free", "Retrig", "Env", "Mono" };
    inline constexpr const char* lfoDir[]            = { "Forward", "Reverse", "Ping-Pong" };
    inline constexpr const char* lfoSync[]           = { "Hz", "Sync" };
    inline constexpr const char* lfoGridX[]          = { "2", "3", "4", "6", "8", "12", "16", "24", "32", "OFF" };
    inline constexpr const char* lfoGridY[]          = { "2", "4", "8", "12", "16", "OFF" };
    inline constexpr const char* voiceMode[]         = { "Poly", "Mono", "Legato" };
    inline constexpr const char* bendRange[]         = { "\xC2\xB1" "2", "\xC2\xB1" "12", "\xC2\xB1" "24" };   // +-2, +-12, +-24

    // 60 entries, 5 groups: WAVE BENDING 0-14, WAVEFOLDING 15-24, DISTORTION 25-43,
    // PHASE & TIME 44-53, MODULATION 54-59.  Default index 17 = "Sine Fold".
    inline constexpr const char* warpMode[] = {
        /*  0 */ "Bend Positive", "Bend Negative", "Bend Symmetric", "Bend Asymmetric", "Asym Positive",
        /*  5 */ "Asym Negative", "Asym Bidirectional", "PWM", "Flip", "Mirror",
        /* 10 */ "Wave Skew", "Wave Stretch", "Wave Compress", "Wave Expand", "Variable Duty Cycle",
        /* 15 */ "Linear Fold", "Multi Fold", "Sine Fold", "Triangle Fold", "Soft Fold",
        /* 20 */ "Hard Fold", "Symmetric Fold", "Asymmetric Fold", "Dynamic Fold", "Chebyshev Fold",
        /* 25 */ "Hard Clip", "Soft Clip", "Tanh", "Arctangent", "Tube Saturation",
        /* 30 */ "Diode 1", "Diode 2", "Cubic", "Sinusoidal", "Exponential",
        /* 35 */ "Asymmetric", "Polynomial", "Chebyshev Harmonic", "Full Wave Rect", "Half Wave Rect",
        /* 40 */ "Soft Rect", "Bitcrush", "SR Reduction", "Quantization", "Phase Distortion",
        /* 45 */ "Phase Stretch", "Phase Compression", "Phase Mirror", "Phase Skew", "Phase Quantization",
        /* 50 */ "Phase Remapping", "Windowed Sync", "Hard Sync", "Soft Sync", "FM",
        /* 55 */ "PM", "AM", "RM", "Self FM", "Cross Mod"
    };

    // 34 entries, 2 groups: STANDARD 0-19, SPECIAL 20-33.  Default index 3 = "LP 24".
    inline constexpr const char* filterType[] = {
        /*  0 */ "LP 6", "LP 12", "LP 18", "LP 24", "LP 36",
        /*  5 */ "LP 48", "HP 6", "HP 12", "HP 18", "HP 24",
        /* 10 */ "HP 36", "HP 48", "BP 6", "BP 12", "BP 24",
        /* 15 */ "Notch", "Var Notch", "Peak", "Low Shelf", "High Shelf",
        /* 20 */ "Comb +", "Comb \xE2\x88\x92", "All Pass", "Serial AP", "Formant",       // index 21 = "Comb -" (U+2212 minus)
        /* 25 */ "Vowel", "Morph Formant", "Phaser", "Res LP", "Res HP",
        /* 30 */ "Dist LP", "Scream", "S&H", "Ring Mod"
    };
}

//==============================================================================
// The table.  LFO blocks are stamped out 10 times; N is passed as a literal so "#N" stringises.
//==============================================================================
#define WARPR_LFO_PARAMS(XF, XI, XB, XC, N) \
    XC (lfo##N##Mode,   "LFO " #N " Mode",   1, lfoMode, true) \
    XC (lfo##N##Dir,    "LFO " #N " Dir",    0, lfoDir,  true) \
    XC (lfo##N##Sync,   "LFO " #N " Sync",   1, lfoSync, true) \
    XF (lfo##N##Rate,   "LFO " #N " Rate",   0.0f, 1.0f,   0.0f, 0.0f, 0.375f, "") \
    XF (lfo##N##Phase,  "LFO " #N " Phase",  0.0f, 360.0f, 1.0f, 0.0f, 0.0f,   "deg") \
    XF (lfo##N##Smooth, "LFO " #N " Smooth", 0.0f, 1.0f,   0.0f, 0.0f, 0.0f,   "") \
    XC (lfo##N##GridX,  "LFO " #N " Grid X", 4, lfoGridX, false) \
    XC (lfo##N##GridY,  "LFO " #N " Grid Y", 2, lfoGridY, false)

#define WARPR_PARAM_TABLE(XF, XI, XB, XC) \
    /* ---------------------------------------------------------------- Sample (8) */ \
    XF (sampleStart,  "Sample Start", 0.0f, 1.0f, 0.001f, 0.0f, 0.0f, "") \
    XF (sampleEnd,    "Sample End",   0.0f, 1.0f, 0.001f, 0.0f, 1.0f, "") \
    XI (rootNote,     "Root Note",    0, 127, 60, "") \
    XI (transpose,    "Transpose",   -24, 24, 0, "st") \
    XF (fineTune,     "Fine Tune",  -100.0f, 100.0f, 0.1f, 0.0f, 0.0f, "cents") \
    XC (loopMode,     "Loop Mode",    0, loopMode,  true) \
    XC (direction,    "Direction",    0, direction, true) \
    XC (playMode,     "Play Mode",    0, playMode,  true) \
    /* ---------------------------------------------------------------- Granular (12) */ \
    XC (grainMode,      "Grain Mode",       0, grainMode, true) \
    XF (grainPosition,  "Grain Position",   0.0f, 1.0f,    0.0f, 0.0f,  0.0f, "") \
    XF (grainScan,      "Grain Scan",       0.0f, 1.0f,    0.0f, 0.0f,  0.0f, "") \
    XF (grainScanDist,  "Grain Scan Dist", -1.0f, 1.0f,    0.0f, 0.0f,  0.0f, "") \
    XB (grainScanLoop,  "Grain Scan Loop",  true) \
    XB (grainScanKey,   "Grain Scan Key",   false) \
    XF (grainSize,      "Grain Size",       2.0f, 2000.0f, 0.0f, 80.0f, 80.0f, "ms") \
    XF (grainShape,     "Grain Shape",      0.0f, 1.0f,    0.0f, 0.0f,  0.5f, "") \
    XF (grainVariation, "Grain Variation",  0.0f, 1.0f,    0.0f, 0.0f,  0.0f, "") \
    XF (grainSpread,    "Grain Spread",     0.0f, 1.0f,    0.0f, 0.0f,  0.0f, "") \
    XF (grainReverse,   "Grain Reverse",    0.0f, 1.0f,    0.0f, 0.0f,  0.0f, "") \
    XI (grainDensity,   "Grain Density",    1, 20, 8, "") \
    /* ---------------------------------------------------------------- Warp (9) */ \
    XB (warpEnable,   "Warp Enable", true) \
    XC (warpMode,     "Warp Mode",   17, warpMode, true) \
    XF (warpAmount,   "Warp Amount",   0.0f, 1.0f,  0.0f, 0.0f, 0.0f, "") \
    XF (warpParamA,   "Warp Param A",  0.0f, 1.0f,  0.0f, 0.0f, 0.5f, "") \
    XF (warpParamB,   "Warp Param B",  0.0f, 1.0f,  0.0f, 0.0f, 0.0f, "") \
    XC (modOscSource, "Mod Osc Source", 4, modOscSource, true) \
    XF (modOscRatio,  "Mod Osc Ratio", 0.25f, 16.0f, 0.0f, 2.0f, 1.0f, "x") \
    XC (oversampling, "Oversampling",  1, oversampling, true) \
    XF (warpMix,      "Warp Mix",      0.0f, 1.0f,  0.0f, 0.0f, 1.0f, "") \
    /* ---------------------------------------------------------------- Shaper (6) */ \
    XB (shaperEnable,       "Shaper Enable",       false) \
    XC (shaperCurveSelect,  "Shaper Curve Select", 0, shaperCurveSelect, true) \
    XF (shaperMorph,        "Shaper Morph", 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, "") \
    XC (shaperPreset,       "Shaper Preset",       8, shaperPreset,      true) \
    XC (shaperSymmetry,     "Shaper Symmetry",     0, shaperSymmetry,    true) \
    XF (shaperMix,          "Shaper Mix",   0.0f, 1.0f, 0.0f, 0.0f, 1.0f, "") \
    /* ---------------------------------------------------------------- Envelope (4) */ \
    XF (ampAttack,  "Amp Attack",  0.0f, 10000.0f, 0.0f,  50.0f,   0.0f, "ms") \
    XF (ampDecay,   "Amp Decay",   1.0f, 10000.0f, 0.0f, 300.0f, 180.0f, "ms") \
    XF (ampSustain, "Amp Sustain", 0.0f,     1.0f, 0.0f,   0.0f,   1.0f, "") \
    XF (ampRelease, "Amp Release", 1.0f, 20000.0f, 0.0f, 400.0f,  50.0f, "ms") \
    /* ---------------------------------------------------------------- Filter (8) */ \
    XB (filterEnable,    "Filter Enable", true) \
    XC (filterType,      "Filter Type",   3, filterType, true) \
    XF (filterCutoff,    "Filter Cutoff",    20.0f, 20000.0f, 0.0f, 1000.0f, 20000.0f, "Hz") \
    XF (filterResonance, "Filter Resonance",  0.0f,     1.0f, 0.0f,    0.0f,     0.0f, "") \
    XF (filterDrive,     "Filter Drive",      0.0f,    24.0f, 0.0f,    0.0f,     0.0f, "dB") \
    XF (filterMorph,     "Filter Morph",      0.0f,     1.0f, 0.0f,    0.0f,     0.0f, "") \
    XF (filterKeyTrack,  "Filter Key Track",  0.0f,     1.0f, 0.0f,    0.0f,     0.0f, "") \
    XF (filterMix,       "Filter Mix",        0.0f,     1.0f, 0.0f,    0.0f,     1.0f, "") \
    /* ---------------------------------------------------------------- LFO 1..10 (80) */ \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 1) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 2) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 3) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 4) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 5) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 6) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 7) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 8) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 9) \
    WARPR_LFO_PARAMS (XF, XI, XB, XC, 10) \
    /* ---------------------------------------------------------------- Global (8) */ \
    XC (voiceMode,     "Voice Mode",     0, voiceMode, true) \
    XI (polyphony,     "Polyphony",      1, 32, 16, "") \
    XI (unisonVoices,  "Unison Voices",  1, 8, 1, "") \
    XF (unisonDetune,  "Unison Detune",  0.0f, 100.0f,  0.1f, 0.0f,   12.0f, "cents") \
    XF (unisonSpread,  "Unison Spread",  0.0f, 1.0f,    0.0f, 0.0f,    0.5f,  "") \
    XF (glideTime,     "Glide Time",     0.0f, 5000.0f, 0.0f, 200.0f,  0.0f,  "ms") \
    XC (bendRange,     "Bend Range",     0, bendRange, true) \
    XF (masterGain,    "Master Gain",  -60.0f, 12.0f,   0.0f, 0.0f,    0.0f,  "dB")

//==============================================================================
namespace warpr
{
    /** Parameter index = position in the APVTS layout (and in kParamIds / kParamMeta). */
   #define WARPR_ENUM_ENTRY(id, ...) id,
    enum class ParamId : int
    {
        WARPR_PARAM_TABLE (WARPR_ENUM_ENTRY, WARPR_ENUM_ENTRY, WARPR_ENUM_ENTRY, WARPR_ENUM_ENTRY)
        count
    };
   #undef WARPR_ENUM_ENTRY

    inline constexpr std::size_t kNumParams = static_cast<std::size_t> (ParamId::count);
    static_assert (kNumParams == 135, "parameter-spec.md defines exactly 135 parameters");

    constexpr std::size_t toIndex (ParamId p) noexcept { return static_cast<std::size_t> (p); }

    enum class ParamKind : std::uint8_t { Float, Int, Bool, Choice };

    struct ParamMeta
    {
        const char* id;
        ParamKind   kind;
        bool        automatable;
        float       minValue;       // denormalised
        float       maxValue;       // denormalised (Choice: numChoices - 1, Bool: 1)
        float       defaultValue;   // denormalised (Choice: index)
        int         numChoices;     // Choice: list size; Bool: 2; otherwise 0
        float       step;           // Float: interval (0 = continuous); Int/Bool/Choice: 1
        float       skewCentre;     // Float: setSkewForCentre value (0 = linear)
    };

   #define WARPR_META_F(id, name, mn, mx, step, centre, def, unit) \
        ParamMeta { #id, ParamKind::Float, true, mn, mx, def, 0, step, centre },
   #define WARPR_META_I(id, name, mn, mx, def, unit) \
        ParamMeta { #id, ParamKind::Int, true, static_cast<float> (mn), static_cast<float> (mx), static_cast<float> (def), 0, 1.0f, 0.0f },
   #define WARPR_META_B(id, name, def) \
        ParamMeta { #id, ParamKind::Bool, true, 0.0f, 1.0f, (def) ? 1.0f : 0.0f, 2, 1.0f, 0.0f },
   #define WARPR_META_C(id, name, def, lst, automatable) \
        ParamMeta { #id, ParamKind::Choice, automatable, 0.0f, \
                    static_cast<float> (std::size (::warpr::choices::lst) - 1), static_cast<float> (def), \
                    static_cast<int> (std::size (::warpr::choices::lst)), 1.0f, 0.0f },

    inline constexpr std::array<ParamMeta, kNumParams> kParamMeta { {
        WARPR_PARAM_TABLE (WARPR_META_F, WARPR_META_I, WARPR_META_B, WARPR_META_C)
    } };

   #undef WARPR_META_F
   #undef WARPR_META_I
   #undef WARPR_META_B
   #undef WARPR_META_C

    constexpr const char* idOf (ParamId p) noexcept { return kParamMeta[toIndex (p)].id; }

    //==========================================================================
    // Compile-time guards on the table shape (spec: Float 62, Int 5, Bool 5, Choice 63;
    // 115 automatable + 20 non-automatable).
    //==========================================================================
    constexpr std::size_t countKind (ParamKind k) noexcept
    {
        std::size_t n = 0;
        for (const auto& m : kParamMeta) if (m.kind == k) ++n;
        return n;
    }

    constexpr std::size_t countAutomatable() noexcept
    {
        std::size_t n = 0;
        for (const auto& m : kParamMeta) if (m.automatable) ++n;
        return n;
    }

    static_assert (countKind (ParamKind::Float)  == 62, "spec: 62 Float");
    static_assert (countKind (ParamKind::Int)    == 5,  "spec: 5 Int");
    static_assert (countKind (ParamKind::Bool)   == 5,  "spec: 5 Bool");
    static_assert (countKind (ParamKind::Choice) == 63, "spec: 63 Choice");
    static_assert (countAutomatable() == 115,           "spec: 115 automatable (20 lfoNGridX/Y are not)");
    static_assert (std::size (choices::warpMode)   == 60, "spec: warpMode has 60 entries");
    static_assert (std::size (choices::filterType) == 34, "spec: filterType has 34 entries");
}
