#pragma once
/*
  FilterTypeTable - one row per filterType choice (architecture section G, "Filter Engine: 34 Types in 9 Families").

  The table covers ALL 34 types in the exact order and spelling of parameter-spec.md / choices::filterType
  (the names are read from that single list, so they can never drift).  A row says what the type IS:

      family       the shared building-block family (architecture "F-xxx" codes)
      build        the phase that implements it: D5a standard 0-19, D5b special linear 20-27, D5c nonlinear 28-33
      implemented  true  : real DSP exists (D5a: indices 0-19)
                   false : NOT YET IMPLEMENTED.  STATIC MAPPING: the type is the IDENTITY (wet = dry, bit-exact)
                           and its analytic response is a flat 0 dB line until its phase fills it in.  Selecting
                           such a type is safe and testable; no fake DSP stands in for it (same rule as WarpModeTable).
      linear       the type is a linear time-invariant system for a fixed parameter set, so FilterResponse is exact.
                   Nonlinear types (28-33, plus Comb/S&H/Ring Mod which are not LTI-exact in the display sense) get a
                   small-signal response in their own phase.
      resRole/morphRole  what the RES / MORPH knobs do ("-" = no effect), shared with the GUI later.

  Namespace-level constexpr data: no static initialisation order issues, no allocation.
*/

#include "Parameters/ParamTable.h"

#include <array>

namespace warpr
{
    enum class FilterFamily : unsigned char
    {
        OnePole,        // F-1P   TptOnePole (6 dB)
        Svf,            // F-SVF  TptSvf (12 dB, LP/BP/HP/notch/peak/shelf taps)
        Cascade,        // F-CAS  one-pole + SVF cascades (18..48 dB), staggered band-pass pairs
        Comb,           // F-COMB
        AllPass,        // F-AP
        Formant,        // F-FRM
        Ladder,         // F-LAD
        Scream,         // F-SCR
        SampleHold,     // F-SAH
        RingMod         // F-RM
    };

    enum class FilterBuild : unsigned char { D5a, D5b, D5c };

    struct FilterTypeInfo
    {
        const char*  name;
        FilterFamily family;
        FilterBuild  build;
        bool         implemented;
        bool         linear;
        const char*  topology;
        const char*  resRole;
        const char*  morphRole;
    };

    inline constexpr int kNumFilterTypes            = 34;
    inline constexpr int kNumImplementedFilterTypes = 20;     // D5a: indices 0-19
    inline constexpr int kNumLowpassTypes           = 6;      // indices 0-5 (LP 6 .. LP 48)

    namespace detail
    {
        constexpr FilterTypeInfo frow (int index, FilterFamily family, FilterBuild build, bool linear,
                                       const char* topology, const char* res, const char* morph)
        {
            return FilterTypeInfo { choices::filterType[index], family, build, build == FilterBuild::D5a, linear,
                                    topology, res, morph };
        }

        using FF = FilterFamily;
        using FB = FilterBuild;
    }

    inline constexpr std::array<FilterTypeInfo, 34> kFilterTypes {{
        // ---- STANDARD (0-19): IMPLEMENTED in D5a (FilterDesign.cpp / FilterStage.cpp) --------------------
        detail::frow (0,  detail::FF::OnePole, detail::FB::D5a, true, "one-pole TPT low-pass",                 "-", "-"),
        detail::frow (1,  detail::FF::Svf,     detail::FB::D5a, true, "TPT SVF low-pass",                      "Q", "Butterworth <-> Bessel-like Q"),
        detail::frow (2,  detail::FF::Cascade, detail::FB::D5a, true, "one-pole + SVF low-pass",               "Q of the SVF", "Butterworth <-> Bessel-like Q"),
        detail::frow (3,  detail::FF::Cascade, detail::FB::D5a, true, "2 x SVF low-pass (Butterworth)",       "Q of the high-Q section", "Butterworth <-> Bessel-like Q"),
        detail::frow (4,  detail::FF::Cascade, detail::FB::D5a, true, "3 x SVF low-pass (Butterworth)",       "Q of the high-Q section", "Butterworth <-> Bessel-like Q"),
        detail::frow (5,  detail::FF::Cascade, detail::FB::D5a, true, "4 x SVF low-pass (Butterworth)",       "Q of the high-Q section", "Butterworth <-> Bessel-like Q"),
        detail::frow (6,  detail::FF::OnePole, detail::FB::D5a, true, "one-pole TPT high-pass",                "-", "-"),
        detail::frow (7,  detail::FF::Svf,     detail::FB::D5a, true, "TPT SVF high-pass",                     "Q", "Butterworth <-> Bessel-like Q"),
        detail::frow (8,  detail::FF::Cascade, detail::FB::D5a, true, "one-pole + SVF high-pass",              "Q of the SVF", "Butterworth <-> Bessel-like Q"),
        detail::frow (9,  detail::FF::Cascade, detail::FB::D5a, true, "2 x SVF high-pass (Butterworth)",      "Q of the high-Q section", "Butterworth <-> Bessel-like Q"),
        detail::frow (10, detail::FF::Cascade, detail::FB::D5a, true, "3 x SVF high-pass (Butterworth)",      "Q of the high-Q section", "Butterworth <-> Bessel-like Q"),
        detail::frow (11, detail::FF::Cascade, detail::FB::D5a, true, "4 x SVF high-pass (Butterworth)",      "Q of the high-Q section", "Butterworth <-> Bessel-like Q"),
        detail::frow (12, detail::FF::OnePole, detail::FB::D5a, true, "one-pole HP at fc/s + one-pole LP at fc*s", "-", "corner spacing s = 1x..8x"),
        detail::frow (13, detail::FF::Svf,     detail::FB::D5a, true, "TPT SVF band-pass",                     "Q 0.5..20", "constant-peak <-> constant-skirt"),
        detail::frow (14, detail::FF::Cascade, detail::FB::D5a, true, "2 x SVF band-pass, staggered",         "Q 0.5..20", "stagger +-morph/2 octave"),
        detail::frow (15, detail::FF::Svf,     detail::FB::D5a, true, "TPT SVF notch",                         "Q (width)", "-"),
        detail::frow (16, detail::FF::Svf,     detail::FB::D5a, true, "x - depth * band-pass",                 "Q (width)", "depth (0 bypass .. 1 full notch)"),
        detail::frow (17, detail::FF::Svf,     detail::FB::D5a, true, "TPT peaking bell",                      "bipolar gain +-18 dB (0.5 = 0 dB)", "bandwidth (Q 4 -> 0.3)"),
        detail::frow (18, detail::FF::Svf,     detail::FB::D5a, true, "TPT low shelf",                         "bipolar gain +-18 dB (0.5 = 0 dB)", "slope (Q 0.5..2)"),
        detail::frow (19, detail::FF::Svf,     detail::FB::D5a, true, "TPT high shelf",                        "bipolar gain +-18 dB (0.5 = 0 dB)", "slope (Q 0.5..2)"),

        // ---- SPECIAL linear (20-27): D5b -----------------------------------------------------------------
        detail::frow (20, detail::FF::Comb,    detail::FB::D5b, true, "positive-feedback comb",                "feedback", "loop damping"),
        detail::frow (21, detail::FF::Comb,    detail::FB::D5b, true, "negative-feedback comb",                "feedback", "loop damping"),
        detail::frow (22, detail::FF::AllPass, detail::FB::D5b, true, "2nd-order all-pass",                    "Q", "blend to 1st order"),
        detail::frow (23, detail::FF::AllPass, detail::FB::D5b, true, "serial 1st-order all-pass chain",       "spread", "stage count"),
        detail::frow (24, detail::FF::Formant, detail::FB::D5b, true, "2+1 formant bank",                      "narrowness", "F2 ratio"),
        detail::frow (25, detail::FF::Formant, detail::FB::D5b, true, "5-formant vowel bank (stepped)",        "narrowness", "vowel"),
        detail::frow (26, detail::FF::Formant, detail::FB::D5b, true, "5-formant vowel bank (interpolated)",   "narrowness", "vowel"),
        detail::frow (27, detail::FF::AllPass, detail::FB::D5b, true, "all-pass phaser",                       "feedback (bipolar)", "stage count"),

        // ---- NONLINEAR (28-33): D5c -----------------------------------------------------------------------
        detail::frow (28, detail::FF::Ladder,     detail::FB::D5c, false, "4-pole ladder low-pass",          "resonance", "pole blend"),
        detail::frow (29, detail::FF::Ladder,     detail::FB::D5c, false, "4-pole ladder high-pass",         "resonance", "pole blend"),
        detail::frow (30, detail::FF::Ladder,     detail::FB::D5c, false, "driven ladder low-pass",          "resonance", "in-loop distortion"),
        detail::frow (31, detail::FF::Scream,     detail::FB::D5c, false, "resonant BP with clipped feedback", "resonance", "scream amount"),
        detail::frow (32, detail::FF::SampleHold, detail::FB::D5c, false, "pre-LP + sample & hold",          "pre-LP", "hold <-> interpolate"),
        detail::frow (33, detail::FF::RingMod,    detail::FB::D5c, false, "carrier multiplier",              "product LP", "AM <-> RM")
    }};

    constexpr const FilterTypeInfo& filterTypeInfo (int index) noexcept
    {
        return kFilterTypes[static_cast<std::size_t> (index < 0 ? 0 : (index >= kNumFilterTypes ? kNumFilterTypes - 1 : index))];
    }
}
