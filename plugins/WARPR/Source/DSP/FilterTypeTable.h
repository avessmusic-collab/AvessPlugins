#pragma once
/*
  FilterTypeTable - one row per filterType choice (architecture section G, "Filter Engine: 34 Types in 9 Families").

  The table covers ALL 34 types in the exact order and spelling of parameter-spec.md / choices::filterType
  (the names are read from that single list, so they can never drift).  A row says what the type IS:

      family       the shared building-block family (architecture "F-xxx" codes)
      build        the phase that implements it: D5a standard 0-19, D5b special linear 20-27, D5c nonlinear 28-33
      implemented  true  : real DSP exists (D5a: indices 0-19, D5b: 20-27, D5c: 28-33 - all 34 types now)
                   false : would mean NOT YET IMPLEMENTED (identity, flat 0 dB); no row uses it any more.
      linear       the type is a linear time-invariant system for a fixed parameter set, so FilterResponse is exact.
                   Nonlinear types (28-33) get the small-signal (linearised) response, see FilterResponse.
      aliasByDesign  aliasing is the effect (S&H): alias gates skip the row.
      localOversampling  the nonlinear core runs at 2 fs between a half-band pair (28-31, 33); S&H runs at the base rate.
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
        bool         aliasByDesign = false;      // D5c: aliasing IS the effect (S&H); the alias gates skip these rows
        bool         localOversampling = false;  // D5c: the nonlinear core runs at 2x behind a half-band pair
    };

    inline constexpr int kNumFilterTypes            = 34;
    inline constexpr int kNumImplementedFilterTypes = 34;     // D5a: 0-19, D5b: 20-27, D5c: 28-33 (all implemented)
    inline constexpr int kNumLowpassTypes           = 6;      // indices 0-5 (LP 6 .. LP 48)

    namespace detail
    {
        constexpr FilterTypeInfo frow (int index, FilterFamily family, FilterBuild build, bool linear,
                                       const char* topology, const char* res, const char* morph,
                                       bool aliasByDesign = false, bool localOs = false)
        {
            return FilterTypeInfo { choices::filterType[index], family, build, true, linear,
                                    topology, res, morph, aliasByDesign, localOs };
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
        detail::frow (20, detail::FF::Comb,    detail::FB::D5b, true, "positive-feedback comb, Lagrange3 delay, loop one-pole",  "feedback g = 0.97 res", "loop damping (bright -> dark)"),
        detail::frow (21, detail::FF::Comb,    detail::FB::D5b, true, "negative-feedback comb (teeth at odd multiples of fc)", "feedback g = 0.97 res", "loop damping (bright -> dark)"),
        detail::frow (22, detail::FF::AllPass, detail::FB::D5b, true, "2nd-order TPT all-pass blended with a 1st-order one", "Q 0.5..20", "blend to 1st order"),
        detail::frow (23, detail::FF::AllPass, detail::FB::D5b, true, "serial 1st-order all-pass chain, stage i at fc 2^(i s)", "spread s = 0.5 res octaves", "stage count 2..16"),
        detail::frow (24, detail::FF::Formant, detail::FB::D5b, true, "3 parallel band-passes: F1 = fc, F2 = F1 x ratio, fixed F3", "narrowness (Q x 1/4..4)", "F2 ratio 1.5..3.5"),
        detail::frow (25, detail::FF::Formant, detail::FB::D5b, true, "5-formant vowel bank A-E-I-O-U (stepped)",        "narrowness (bandwidth x 4..1/4)", "vowel (plateaus)"),
        detail::frow (26, detail::FF::Formant, detail::FB::D5b, true, "5-formant vowel bank A-E-I-O-U (interpolated)",   "narrowness (bandwidth x 4..1/4)", "vowel (continuous)"),
        detail::frow (27, detail::FF::AllPass, detail::FB::D5b, true, "all-pass chain + dry sum with one-sample feedback",  "feedback -0.95..+0.95 (0.5 = 0)", "stage count 2..12"),

        // ---- NONLINEAR (28-33): D5c -----------------------------------------------------------------------
        // Local 2x: the core runs at 2 fs between a polyphase half-band interpolator and decimator (FilterNonlinear.h).
        detail::frow (28, detail::FF::Ladder,     detail::FB::D5c, false, "4-stage TPT ladder low-pass, saturator per stage, local 2x",
                      "feedback k = 4.2 res (self-oscillates above res 0.95)", "tap blend 2-pole (0) <-> 4-pole (1)", false, true),
        detail::frow (29, detail::FF::Ladder,     detail::FB::D5c, false, "4-stage TPT ladder high-pass, taps (1,-4,6,-4,1), local 2x",
                      "feedback k = 4.2 res (self-oscillates above res 0.95)", "tap blend 2-pole (0) <-> 4-pole (1)", false, true),
        detail::frow (30, detail::FF::Ladder,     detail::FB::D5c, false, "4-pole ladder low-pass, drive-coupled in-loop saturation, local 2x",
                      "feedback k = 4.2 res", "in-loop distortion (saturation knee 1 -> 1/16)", false, true),
        detail::frow (31, detail::FF::Scream,     detail::FB::D5c, false, "peak-normalised SVF band-pass, 0-40 dB input gain, asymmetric clip, clipped feedback, local 2x",
                      "Q 0.7 .. 28", "scream (input gain 0..40 dB and feedback 0..0.95 together)", false, true),
        detail::frow (32, detail::FF::SampleHold, detail::FB::D5c, false, "2 x one-pole pre-LP, sample & hold at fc, base rate",
                      "anti-alias pre-LP (open -> 0.4 x hold rate)", "hold (0) <-> linear interpolation (1)", true, false),
        detail::frow (33, detail::FF::RingMod,    detail::FB::D5c, false, "carrier multiplier + 2 x one-pole product LP, local 2x",
                      "product LP (open -> max (fc/2, 40 Hz))", "AM (0, unipolar carrier) <-> RM (1, bipolar)", false, true)
    }};

    constexpr const FilterTypeInfo& filterTypeInfo (int index) noexcept
    {
        return kFilterTypes[static_cast<std::size_t> (index < 0 ? 0 : (index >= kNumFilterTypes ? kNumFilterTypes - 1 : index))];
    }
}
