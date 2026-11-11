#pragma once
/*
  WarpModeTable - one row per warpMode choice (architecture "Warp state machine").

  The table covers ALL 60 modes in the exact order and spelling of parameter-spec.md /
  choices::warpMode (PhaseD2Tests compares every name).  A row says what the mode IS:

      family     the shared-infrastructure family (architecture "Family map")
      kind       Phase  : adds a read-position offset  P' = P + dir * Lc * Delta(theta)  (before the read)
                 Amp    : shapes the value read from the sample
      implemented  true  : real DSP exists (D2: indices 0-14)
                   false : NOT YET IMPLEMENTED.  STATIC MAPPING: the mode is the IDENTITY (wet = dry,
                           bit-exact) until its phase fills it in:
                             15-43  folds / distortions / rectifiers / quantisers ... D3
                             44-59  phase & time, sync, modulation ........................ D6
                           Selecting such a mode is safe and testable; no fake DSP stands in for it.
      minOS      oversampling hint (UI/governor only; never forces the factor)
      stepFlag   the mode produces an INTENDED value discontinuity (T1 discontinuity test uses the
                 mode's own bound): Flip (polarity edges), Wave Skew (sawtooth wrap)
      seamJump   g(1) != g(0)+1 for a > 0 (read position jumps at the cycle wrap): the SeamBridge
                 splits the resulting output step over a few samples (Mirror)
      aRole/bRole  what the A / B knobs do ("-" = no effect), shared with the GUI later (WarpModeInfo)

  Namespace-level constexpr data: no static initialisation order issues, no allocation.
*/

#include <array>

namespace warpr
{
    enum class WarpFamily : unsigned char
    {
        H,      // phase-map bends
        V,      // amplitude power / asymmetric bends
        VT,     // cycle-aware amplitude (theta-gated)
        Fold, Dyn, Clip, Rect, Qnt,
        Phm,    // phase & time maps
        Syn,    // sync
        Mod     // FM / PM / AM / RM / Self FM / Cross Mod
    };

    enum class WarpKind : unsigned char { Phase, Amp };

    struct WarpModeInfo
    {
        const char* name;
        WarpFamily  family;
        WarpKind    kind;
        bool        implemented;
        int         minOS;
        bool        stepFlag;
        bool        seamJump;
        const char* aRole;
        const char* bRole;
    };

    namespace detail
    {
        constexpr WarpModeInfo row (const char* name, WarpFamily f, WarpKind k, bool impl, int minOs,
                                    bool step, bool seam, const char* a, const char* b)
        {
            return WarpModeInfo { name, f, k, impl, minOs, step, seam, a, b };
        }

        constexpr WarpModeInfo todo (const char* name, WarpFamily f, WarpKind k, int minOs)
        {
            return row (name, f, k, false, minOs, false, false, "-", "-");
        }

        using F = WarpFamily;
        using K = WarpKind;
    }

    inline constexpr int kNumWarpModes = 60;
    inline constexpr int kNumImplementedWarpModes = 15;      // D2: indices 0-14

    inline constexpr std::array<WarpModeInfo, 60> kWarpModes {{
        // ---- WAVE BENDING (0-14): IMPLEMENTED in D2 ---------------------------------------------------
        detail::row ("Bend Positive",      detail::F::H,  detail::K::Phase, true, 1, false, false, "exponent range",        "-"),
        detail::row ("Bend Negative",      detail::F::H,  detail::K::Phase, true, 1, false, false, "exponent range",        "-"),
        detail::row ("Bend Symmetric",     detail::F::H,  detail::K::Phase, true, 1, false, false, "exponent range",        "-"),
        detail::row ("Bend Asymmetric",    detail::F::H,  detail::K::Phase, true, 1, false, false, "1st-half exponent range", "2nd-half amount"),
        detail::row ("Asym Positive",      detail::F::V,  detail::K::Amp,   true, 2, false, false, "strength",              "-"),
        detail::row ("Asym Negative",      detail::F::V,  detail::K::Amp,   true, 2, false, false, "strength",              "-"),
        detail::row ("Asym Bidirectional", detail::F::V,  detail::K::Amp,   true, 2, false, false, "positive depth",       "negative depth"),
        detail::row ("PWM",                detail::F::H,  detail::K::Phase, true, 2, false, false, "window position",       "edge softness"),
        detail::row ("Flip",               detail::F::VT, detail::K::Amp,   true, 2, true,  false, "region offset",         "edge softness"),
        detail::row ("Mirror",             detail::F::H,  detail::K::Phase, true, 2, false, true,  "-",                     "apex rounding"),
        detail::row ("Wave Skew",          detail::F::VT, detail::K::Amp,   true, 2, true,  false, "ramp curvature",        "ramp pivot / sine lean"),
        detail::row ("Wave Stretch",       detail::F::V,  detail::K::Amp,   true, 2, false, false, "knee blend",            "-"),
        detail::row ("Wave Compress",      detail::F::V,  detail::K::Amp,   true, 2, false, false, "threshold blend",       "-"),
        detail::row ("Wave Expand",        detail::F::V,  detail::K::Amp,   true, 2, false, false, "threshold blend",       "-"),
        detail::row ("Variable Duty Cycle", detail::F::H, detail::K::Phase, true, 1, false, false, "corner rounding",       "duty centre offset"),

        // ---- WAVEFOLDING (15-24): D3 -----------------------------------------------------------------
        detail::todo ("Linear Fold",       detail::F::Fold, detail::K::Amp, 2),
        detail::todo ("Multi Fold",        detail::F::Fold, detail::K::Amp, 2),
        detail::todo ("Sine Fold",         detail::F::Fold, detail::K::Amp, 2),
        detail::todo ("Triangle Fold",     detail::F::Fold, detail::K::Amp, 2),
        detail::todo ("Soft Fold",         detail::F::Fold, detail::K::Amp, 2),
        detail::todo ("Hard Fold",         detail::F::Fold, detail::K::Amp, 2),
        detail::todo ("Symmetric Fold",    detail::F::Fold, detail::K::Amp, 2),
        detail::todo ("Asymmetric Fold",   detail::F::Fold, detail::K::Amp, 2),
        detail::todo ("Dynamic Fold",      detail::F::Dyn,  detail::K::Amp, 2),
        detail::todo ("Chebyshev Fold",    detail::F::Fold, detail::K::Amp, 4),

        // ---- DISTORTION (25-43): D3 ------------------------------------------------------------------
        detail::todo ("Hard Clip",         detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Soft Clip",         detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Tanh",              detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Arctangent",        detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Tube Saturation",   detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Diode 1",           detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Diode 2",           detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Cubic",             detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Sinusoidal",        detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Exponential",       detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Asymmetric",        detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Polynomial",        detail::F::Clip, detail::K::Amp, 2),
        detail::todo ("Chebyshev Harmonic", detail::F::Clip, detail::K::Amp, 4),
        detail::todo ("Full Wave Rect",    detail::F::Rect, detail::K::Amp, 2),
        detail::todo ("Half Wave Rect",    detail::F::Rect, detail::K::Amp, 2),
        detail::todo ("Soft Rect",         detail::F::Rect, detail::K::Amp, 2),
        detail::todo ("Bitcrush",          detail::F::Qnt,  detail::K::Amp, 1),
        detail::todo ("SR Reduction",      detail::F::Qnt,  detail::K::Amp, 1),
        detail::todo ("Quantization",      detail::F::Qnt,  detail::K::Amp, 2),

        // ---- PHASE & TIME (44-53): D6 ----------------------------------------------------------------
        detail::todo ("Phase Distortion",  detail::F::Phm, detail::K::Phase, 2),
        detail::todo ("Phase Stretch",     detail::F::Phm, detail::K::Phase, 2),
        detail::todo ("Phase Compression", detail::F::Phm, detail::K::Phase, 2),
        detail::todo ("Phase Mirror",      detail::F::Phm, detail::K::Phase, 2),
        detail::todo ("Phase Skew",        detail::F::Phm, detail::K::Phase, 2),
        detail::todo ("Phase Quantization", detail::F::Phm, detail::K::Phase, 2),
        detail::todo ("Phase Remapping",   detail::F::Phm, detail::K::Phase, 2),
        detail::todo ("Windowed Sync",     detail::F::Syn, detail::K::Phase, 4),
        detail::todo ("Hard Sync",         detail::F::Syn, detail::K::Phase, 4),
        detail::todo ("Soft Sync",         detail::F::Syn, detail::K::Phase, 2),

        // ---- MODULATION (54-59): D6 ------------------------------------------------------------------
        detail::todo ("FM",                detail::F::Mod, detail::K::Phase, 4),
        detail::todo ("PM",                detail::F::Mod, detail::K::Phase, 4),
        detail::todo ("AM",                detail::F::Mod, detail::K::Amp,   2),
        detail::todo ("RM",                detail::F::Mod, detail::K::Amp,   2),
        detail::todo ("Self FM",           detail::F::Mod, detail::K::Phase, 4),
        detail::todo ("Cross Mod",         detail::F::Mod, detail::K::Phase, 4)
    }};

    /** Row for a warpMode index (clamped into 0..59). */
    inline const WarpModeInfo& warpModeInfo (int index) noexcept
    {
        const int i = index < 0 ? 0 : (index >= kNumWarpModes ? kNumWarpModes - 1 : index);
        return kWarpModes[static_cast<std::size_t> (i)];
    }
}
