#pragma once
/*
  ShaperPresetLibrary - the nine analytic transfer curves behind the shaperPreset choice
  (parameter-spec: Soft Sat | Hard Clip | Tube | Diode | Fold | Sine | Triangle | Expo | Custom).

  Every preset is a POINT SET in the curve wire format (so it can be shown, dragged and persisted by the editor
  like a hand-drawn curve).  generate(i) returns the full list x = -1 .. 1 (odd-symmetric presets list both halves,
  so they are correct in Symmetric AND Asymmetric mode).  The point data:

   0 Soft Sat   smooth, 9 pts  x = -1, -.75 ... 1 (step .25)   y = tanh(2.5 x) / tanh(2.5)
   1 Hard Clip  linear, 4 pts  (-1,-1) (-.5,-1) (.5,1) (1,1)   a slope-2 clipper with corners ON the 1/2048 LUT grid
   2 Tube       smooth, 11 pts x = -1 .. 1 (step .2)           y = n (tanh(2.2 (x + .15)) - tanh(.33)),  n = 1 / max|y|
                (asymmetric: the grid-bias tanh clips the negative side first; +0.526 / -1.0 at the ends)
   3 Diode      smooth, 11 pts x = -1 -.7 -.4 -.2 -.1 0 .1 .2 .4 .7 1
                y = (1 - e^(-3 x)) / (1 - e^-3)                for x >= 0
                y = -0.6 (1 - e^(-2.1 |x|)) / (1 - e^-2.1)     for x <  0     (asymmetric)
   4 Fold       linear, 13 pts triangle fold, two reversals per side: u = 4 x, y = u (|u| <= 1), 2 sgn(u) - u
                (1 < |u| <= 3), u - 4 sgn(u) (|u| > 3).  Corners at x = +-.25, +-.75 (on the LUT grid); the points
                at x = 0, +-.125, +-.5, +-.875 lie on the straight segments (kept so the editor has handles).
   5 Sine       smooth, 9 pts  x = -1 ... 1 (step .25)         y = sin(pi x / 2)
   6 Triangle   linear, 9 pts  zigzag with period 1, amplitude 1: y = 0 1 0 -1 0 1 0 -1 0 at x = -1 ... 1 (step .25),
                i.e. (.25, 1) (.5, 0) (.75, -1) (1, 0) on the positive side
   7 Expo       smooth, 9 pts  x = -1 ... 1 (step .25)         y = sgn(x) (e^(3|x|) - 1) / (e^3 - 1)
   8 Custom     no-op: selecting it changes nothing (it is also what an edited curve flips the parameter to)

  Odd presets (all but Tube and Diode) are built as y(-x) = -y(x) bit-exactly.  isAsymmetric() presets are only
  audible as designed with shaperSymmetry = Asymmetric; the backend switches the parameter when it loads one
  (architecture ambiguity A6).  The linear presets' control points sit on the 1/2048 LUT grid, so the LUT reproduces
  them exactly (a corner inside a cell would cost up to slope-jump * h / 4 of interpolation error).

  Message thread only (allocates).
*/

#include "DSP/ShaperCurve.h"

namespace warpr::shaperpresets
{
    inline constexpr int kNumPresets  = 9;
    inline constexpr int kCustomIndex = 8;

    /** Choice-list spelling (== choices::shaperPreset, checked by the tests). */
    const char* name (int preset) noexcept;

    /** True for presets that regenerate a curve (everything except Custom). */
    bool isLoadable (int preset) noexcept;

    /** True for presets that are not odd-symmetric (Tube, Diode). */
    bool isAsymmetric (int preset) noexcept;

    /** The preset's curve; Custom (and out-of-range) returns the identity. */
    ShaperCurve generate (int preset);
}
