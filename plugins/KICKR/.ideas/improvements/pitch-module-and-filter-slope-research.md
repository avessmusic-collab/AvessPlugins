# Research: Pitch Module + Filter Slope — Implementation Approaches

**Date:** 2026-09-07
**Level reached:** 2 (Quick Check + JUCE forum / EarLevel / GitHub web research)
**Confidence:** HIGH on all three questions (Level 2 confirmed Level 1 and added refinements below)
**Brief:** [pitch-module-and-filter-slope.md](pitch-module-and-filter-slope.md)

---

## 1. Variable-slope master filter (12/18/24 dB/oct)

**Current code:** `Source/DSP/MasterFilter.{h,cpp}` — one `juce::dsp::StateVariableTPTFilter<float>`
(2ch), per-sample `processSample`, cutoff smoothed 20 ms, `svf.reset()` on off→on,
resonance mapped `Q = 0.5 · 10^(1.4·res)`.

**Approach (HIGH confidence):** cascade stages inside `MasterFilter`, keep the API identical
(`setParams` gains a `slope` int):

| Slope | Stages | Resonance |
|-------|--------|-----------|
| 12 dB | svf1 (existing — unchanged path, bit-exact) | user Q on svf1 |
| 18 dB | svf1 + `juce::dsp::FirstOrderTPTFilter<float>` (6 dB) | user Q on svf1; one-pole has none |
| 24 dB | svf1 + svf2 | user Q on svf1; svf2 fixed Q ≈ 0.707 (Butterworth-ish, avoids double-resonance blowup) |

- **`FirstOrderTPTFilter` confirmed present** in the installed JUCE at
  `/Applications/JUCE/modules/juce_dsp/processors/juce_FirstOrderTPTFilter.h`
  (types: lowpass/highpass — matches both filter modes). No extra module needed
  (`juce_dsp` already linked).
- Both extra stages share the same smoothed cutoff value already computed per sample —
  set cutoff on all active stages when it changes (same `lastCutoff` guard).
- All stages prepared once in `prepare()`; slope switch selects the processing path only —
  no allocation on the audio thread.
- **Click-free switching:** on slope change, `reset()` the newly-entering stages only
  (svf1 keeps its state so 12→18/24 doesn't glitch the base path). This mirrors the
  existing off→on reset pattern. If audible ticks remain when switching mid-note,
  fall back to full reset — on a kick synth retriggering every beat this is negligible.
- Keep `snapToZero()` per sample on active stages (existing denormal hygiene).
- **True-Butterworth alternative (rejected):** 24 dB Butterworth uses stage Qs 0.541/1.307;
  with user resonance layered on top it's harder to reason about and the 0.707 second stage
  is the standard "musical resonant cascade" used by Ableton-style filters.

## 2. Pitch-curve log→exp morph

**Current code:** `Source/DSP/PitchEnvelope.h` — `k = 0.6 + c·8.4`, easing
`e(τ) = (exp(−kτ) − exp(−k)) / (1 − exp(−k))`, frequency `f = fund · start^e(τ)`
(log-domain, never Hz-lerp). The current family spans **near-linear (k=0.6) → sharp
exponential (k=9)** — there is no logarithmic (slow-start) half today.

**Approach (HIGH confidence):** split the knob at 0.5, mirror the easing for the log half:

```
c >= 0.5 (exp half — current family, rescaled):
    k = 0.6 + (c − 0.5) · 2 · 8.4
    e(τ) = (exp(−kτ) − exp(−k)) / (1 − exp(−k))          // fast start, slow settle

c < 0.5 (log half — time-mirrored easing):
    k = 0.6 + (0.5 − c) · 2 · 8.4
    e(τ) = 1 − (exp(−k(1−τ)) − exp(−k)) / (1 − exp(−k))  // slow start, fast end
```

- Continuous at c = 0.5 (both sides → k = 0.6 ≈ linear drop). e(0)=1, e(1)=0 hold on
  both halves, so the contour still lands exactly on the fundamental at τ = 1 —
  BodyOscillator phase integration untouched, click-free by construction preserved.
- Same per-block coefficient precompute in `setParams()` (add a `mirrored` bool);
  per-sample cost unchanged (one `exp` + one `exp2`).
- **Preset impact:** old c values ≥ 0.5 get a gentler k than before (e.g. c=0.7: k 6.48 → 3.96);
  values < 0.5 change character to log. Brief already accepts this; factory presets need
  re-audition (already a testing criterion).
- Default stays 0.7 (now a moderate exponential — closest match to old intent).

## 3. Editor changes (PUNCH → PITCH module)

**Current code:** `PluginEditor.cpp:234-241` builds the module — `heroTitles[0]` "PUNCH",
`heroBig[0]` = macroPunch Large, `punchFooter` = {T ATK, T SUS} Small. Pitch knobs live in
`bodyFooter` (lines 250-252). Footer arrays are laid out in `resized()` (~line 906-915).
Knob sizes: `KickrKnob::Size {Small, Medium, Large}` (`UI/KickrKnob.h:84`).

**Approach (HIGH confidence):**
- Relabel section to "PITCH" (keep `pal::red`); `heroBig[0]` = `pitchStart` as **AMOUNT**
  (Large); `punchFooter` = {TIME (`pitchTime`), CURVE (`pitchCurve`), T ATK, T SUS}.
- Remove the three pitch knobs from `bodyFooter` (BODY footer shrinks to LEVEL/DECAY/HARM —
  same count as CRUSH/TAIL footers, layout math already handles per-module counts).
- **Smaller T ATK / T SUS:** two options — (a) add `Size::Tiny` to the enum + KickrKnob
  draw scale, or (b) keep `Small` and shrink only their bounds in `resized()` footer loop
  (special-case the last two entries of `punchFooter`). Option (b) is less invasive if
  KickrKnob renders proportionally to its bounds; check `KickrKnob::paint` — if text/tick
  sizes are bound-relative it works for free. Otherwise add the enum tier.
- **macroPunch retention:** keep the parameter in `ParameterLayout.h` (state/preset/
  automation-lane compatibility — removing an APVTS param breaks old session restore
  silently). Just don't create a knob for it. **Neutralize its DSP contribution** in
  `KickEngine` (treat as the 0.5 default regardless of stored value) so a hidden restored
  value can't shape the sound with no visible control. Mark it
  `.withAutomatable(false)` if the layout builder supports attributes, optional.
- FILTER page gains the slope selector (12/18/24) next to LP/HP; `filterSlope` is a new
  `AudioParameterChoice`, default index 1 (18 dB). `UI/FilterDisplay` curve drawing should
  read the slope to steepen the drawn roll-off.
- **Migration guard (recommended in brief):** in `setStateInformation`, if the restored
  tree predates `filterSlope`, force it to 12 dB so old sessions keep their sound.

## Level 2 refinements (web research)

**Cascade Q values — two viable resonance schemes** (cascading confirmed as the standard
approach on the JUCE forum; a cascade of two Butterworth stages is *not* Butterworth
unless stage Qs are corrected — EarLevel):

- **Scheme A — "musical" (Level 1 plan, still recommended):** user resonance stays on
  svf1 with the existing `Q = 0.5·10^(1.4·res)` map for every slope; companion stages
  neutral (one-pole for 18 dB; svf2 at fixed Q ≈ 0.707 for 24 dB). Pros: res knob feels
  identical across slopes, 12 dB path bit-exact, simplest. Con: at min resonance the
  compound passband is slightly droopier than true Butterworth — but KICKR's res=0 already
  maps to Q=0.5 (below 0.707, deliberately Ableton-ish), so "flat at zero res" was never
  the design point.
- **Scheme B — Butterworth-correct at zero res:** stage Qs per EarLevel — 18 dB: biquad
  Q=1.0 + first-order pole; 24 dB: Q=0.541 and Q=1.307, with user resonance scaling the
  higher-Q stage. Textbook-flat passband, but the resonance knob then behaves differently
  per slope and the res→Q map needs per-slope rebasing. Not worth it for this filter's
  character; documented for reference.

**Click-free switching (forum-confirmed):** TPT structures are the artifact-resistant
choice under fast cutoff modulation (that's why the current filter already survives the
20 ms smoothed cutoff drag). Topology changes (slope switches) are a different animal —
IIR-pop threads show even crossfading doesn't fully cure mid-note structure swaps. Plan
stands: reset entering stages only, accept the corner case (kick retriggers per beat, so
any residual tick lives for one hit at most).

**State compatibility (confirmed by KICKR's own code):** `setStateInformation`
(`PluginProcessor.cpp:168-188`) already does `replaceState` and its v1→v2 migration
comment confirms JUCE applies defaults for params missing from an old tree. So:
- Old sessions load fine with `filterSlope` at its default (18) — the **migration guard**
  is: after `replaceState`, if the restored tree has no `filterSlope` entry (or
  `stateVersion` predates it), set `filterSlope` to the 12 dB index. `kStateVersion`
  already exists as the hook — bump it.
- Keeping `macroPunch` registered (knob removed, DSP contribution neutralized) is the
  right call — the forum threads on removing APVTS parameters all warn against dropping
  registered params for exactly this session-restore reason.

**Pitch curve direction (design confirmation):** kick-synthesis references consistently
describe the punchy 808-style drop as exponential (fast start), with slower/log-leaning
decay curves giving the laid-back tuned-808 character — which matches the planned knob:
0 = log (lazy/tuned), 0.5 = linear, 1 = exp (punch). No prior art found for the
time-mirrored easing specifically; it's sound math (continuous at midpoint, e(0)=1,
e(1)=0 preserved) and stays a KICKR-original detail.

## Sources

- Local: `MasterFilter.{h,cpp}`, `PitchEnvelope.h`, `PluginProcessor.cpp`,
  `PluginEditor.cpp`, `KickrKnob.h`, `ParameterLayout.h` (all read this session)
- Installed JUCE 8 at `/Applications/JUCE` — `FirstOrderTPTFilter` + `StateVariableTPTFilter` confirmed
- `troubleshooting/patterns/juce8-critical-patterns.md` (no conflicts found)
- [JUCE forum: Higher order & slope filters](https://forum.juce.com/t/higher-order-slope-filters-in-juce-framework/56625)
- [EarLevel: Cascading filters (stage Q values)](https://www.earlevel.com/main/2016/09/29/cascading-filters/)
- [JUCE forum: IIR filter pops when state changes (even with crossfading)](https://forum.juce.com/t/iir-filter-pops-when-state-changes-even-with-crossfading/44820)
- [JUCE forum: patch loading with backwards compatibility](https://forum.juce.com/t/how-to-handle-patch-loading-with-backwards-compatibility/37421)
- [JUCE forum: request for APVTS removeParameter (why params must stay registered)](https://forum.juce.com/t/request-for-audioprocessorvaluetreestate-removeparameter/19173)
- [StateVariableTPTFilter docs](https://docs.juce.com/master/classdsp_1_1StateVariableTPTFilter.html)
- Kick-synthesis pitch-curve references: [Perfect Circuit](https://www.perfectcircuit.com/signal/kick-drum-synthesis), [ModeAudio](https://modeaudio.com/magazine/drum-synth-sound-design-kick-snare), [Sound On Sound](https://www.soundonsound.com/techniques/designing-kicks-logic-pro-x)

## Risks / open items

- KickrKnob bounds-proportional rendering unverified — decides Tiny-tier vs bounds-shrink (5-min check at implementation time).
- Factory presets re-audition after curve remap (accepted in brief).
- Slope-switch click behavior to verify by ear; fallback documented above.
