# Level 2 Research Report: Morph Warp-Mode DSP Algorithms (KICKR)

**Improvement doc:** `plugins/KICKR/.ideas/improvements/morph-warp-modes.md`

## Investigation Summary
- **Level:** 2 (Level 1 found no local knowledge-base hits — this is DSP algorithm
  design, not a JUCE API/build problem, so Context7/JUCE-forum/GitHub-issue search
  wasn't the right tool; general web search on the DSP literature was used instead)
- **Sources consulted:** 6 web searches, ~25 distinct sources (Wikipedia, KVR Audio DSP
  forum, ResearchGate, DAFx paper archive, electricdruid.net, ringbuffer.org, Perfect
  Circuit, Gearspace, plutiedev.com)
- **Key finding:** all 6 new modes map to well-documented, well-understood classical
  DSP techniques. None require novel research — the real design work is *taming* each
  technique for a single-cycle-ish percussive body oscillator rather than a sustained
  synth voice, per the brief's "musical/tame ceiling" requirement.

## Detailed Findings

### 1. Bend/Skew (existing — no research needed)
Already implemented (`KickVoice.cpp` `setMorph`). No change.

### 2. Sync
Hard sync resets the slave (body) oscillator's phase every time a faster master phasor
completes a cycle, so the body osc's *effective* fundamental locks to the master rate
while its waveform still carries harmonic content related to the original body pitch.
The risk is aliasing: a phase reset is a discontinuity, and a naive reset aliases
badly, especially since KICKR's body oscillator is already documented as naive/non-
band-limited (`RunTests.cpp` line ~366-368). The established fix is **PolyBLEP**: at
the reset instant, subtract a small polynomial correction from 1-2 samples around the
discontinuity to cancel the aliasing energy, without oversampling. KICKR already runs
its whole voice through 1x/2x/4x/8x oversampling, which independently helps here too —
so a straightforward hard-reset-plus-existing-oversampling implementation may already
be "good enough," with PolyBLEP as a fallback if 2.8 phase profiling shows audible
aliasing at low oversampling settings.
- Master:slave ratio should be the "amount" parameter's mapping target — amount=0 ≈
  1:1 (no audible sync), amount=1 ≈ a musically capped ratio (e.g. up to ~4:1-6:1
  rather than an extreme 20:1) to satisfy the "tame ceiling" requirement.

### 3. Fold (wavefolding)
Wavefolding is waveshaping through a *periodic* transfer function: past a threshold,
increasing input no longer increases output but folds back down, adding odd harmonics
progressively as gain increases. The concrete formula found: **`y = sin(gain * x)`**
(a "sine fold") is a simple, cheap, DSP-standard approach — as internal pre-gain
increases, the output folds back on itself more times per cycle, and the folding
spectrum can be derived analytically via the Jacobi–Anger expansion (confirms it's a
well-behaved, always-bounded function — output of `sin()` is inherently limited to
[-1, 1], which is convenient for staying bounded/finite regardless of gain, directly
satisfying the plugin's existing "no runaway output" test requirement).
- **Recommended mapping:** `amount` (0-1) → pre-gain multiplier, e.g. `gain = 1 +
  amount * K` for some tuned constant `K` (start conservative, e.g. K=4-6, and listen —
  this is the "musical ceiling" tuning called out as an open implementation-time task
  in the improvement brief).
- Because `sin(gain*x)` is smooth (no hard corners), it needs less anti-aliasing
  attention than Sync's hard reset, but higher gain still pushes energy toward Nyquist,
  so it still benefits from the existing oversampling chain.

### 4. PD (Phase Distortion, Casio CZ-style)
Distinct from Bend/Skew's continuous phase-skew: classic CZ implementations use a
**piecewise-linear phasor remap** — e.g. a simple two-segment function that speeds
through the first part of the cycle and slows through the second, read through a
sine/cosine lookup, producing sawtooth-like harmonic content from a pure sine
oscillator without a filter. The "DCW" parameter in the original CZ controls how much
of this remapping is applied, with minimum = pure sine and maximum = the fully bent
shape — this maps naturally onto KICKR's `amount` knob (0 = sine, 1 = fully bent).
- **Differentiator from Bend/Skew:** Bend/Skew (as currently implemented) only skews
  the *attack*, always relaxing to a clean sine in the tail. A true PD mode should
  apply the piecewise-linear phase remap continuously (attack and tail both bent while
  `amount > 0`), giving a genuinely different sustained-vs-transient character — this
  is the most useful way to make PD feel distinct from Bend/Skew rather than redundant.
- Piecewise-linear breakpoint position (where segment 1 ends and segment 2 begins) is
  a natural second axis (this is what CZ's "resonant" PD algorithms vary) — out of
  scope for v1 per the brief (no new ratio/shape sub-parameters), but worth flagging
  as a natural v2 extension.

### 5 & 6. FM and FM-from-Sample
Two-operator FM (carrier = body oscillator, modulator = second sine at a fixed
ratio of body pitch) is the standard kick-synthesis FM topology: a modulator
sine's frequency and index shape the carrier's instantaneous phase, producing
inharmonic/clangy content proportional to modulation index. For a percussive kick
context specifically, published kick-FM patches consistently pair the FM index with
its own short decay envelope so the "clang" is concentrated at the transient and
decays faster than the fundamental — worth considering even though the brief's
`amount` knob is a static per-note control (the *existing* body-oscillator amplitude
envelope KICKR already has for the attack-only Bend/Skew mode could double as this
envelope source for FM's index, keeping implementation self-contained).
- **FM (internal modulator):** confirms the brief's chosen approach — fixed-ratio,
  pitch-tracked internal sine modulator. Index (amount → modulation index mapping)
  needs the same "musical ceiling" tuning as Fold: FM index has no natural bound the
  way `sin()` folding does, so amount=1 must be explicitly clamped to a tuned max
  index rather than left unbounded.
- **FM-from-Sample:** structurally identical FM math, but the modulator signal is
  the Sample layer's current output instead of an internal sine. This is a
  project-architecture question rather than a DSP-research one (confirmed in the
  improvement doc's Implementation Notes: it requires exposing the Sample layer's
  per-sample output to the body oscillator's processing, which today are independent
  layers mixed later). No published technique changes this — it's the same FM
  multiply, just fed a different, non-sinusoidal (and non-band-limited, since it's a
  real sample) modulator signal, which will produce noisier/more complex sidebands
  than the plain-sine FM mode. Because the modulator amplitude is uncontrolled
  (it's whatever the sample's waveform is, not a normalized oscillator), extra
  care is needed clamping/normalizing the sample's instantaneous level before it
  drives the index, to avoid the "musical ceiling" being blown out on loud samples.
  Confirmed fallback for an empty/disabled Sample layer (index scenario the test
  criteria already call out): treat missing sample as modulator level 0 → this mode
  degrades gracefully to "no effect" (equivalent to amount having no audible effect)
  rather than producing silence-driven artifacts.

### 7. AM
Standard amplitude modulation: multiply the carrier by a **unipolar** (shifted-
positive) version of the modulator, so the carrier's original frequency is preserved
in the output alongside sidebands at half the carrier's amplitude — sounds "less
extreme," more like tremolo/vibrato-adjacent coloration than RM. Implementation is the
same fixed-ratio internal sine modulator as FM, just used multiplicatively with a
`0.5 * (1 + sin(...))`-style unipolar shift instead of driving phase.

### 8. RM
Ring modulation: multiply the carrier by the modulator **unshifted** (bipolar,
4-quadrant multiply) — this cancels out the original carrier frequency entirely,
leaving only sum/difference sidebands, producing the characteristic metallic/bell-like
RM timbre. This is the more extreme sibling of AM using the exact same internal
fixed-ratio modulator oscillator — AM and RM can share almost all of their
implementation, differing only in whether the modulator is unipolar-shifted (AM) or
left bipolar (RM) before the multiply. This is a cheap way to implement 2 of the 8
modes together.

## Confidence Assessment
- **Level:** HIGH for algorithm choice per mode (all 6 new modes are standard,
  well-documented DSP techniques with concrete, cheap formulas found above).
- **Level:** MEDIUM for the "musical/tame ceiling" tuning constants (K in Fold's
  gain mapping, FM/AM/RM's max index, Sync's max ratio) — these are inherently a
  by-ear tuning pass during implementation, not something a DSP reference can specify
  in advance. Flag this explicitly for implementation time, matching what the
  improvement brief already anticipated.
- **Level:** MEDIUM for FM-from-Sample specifically, not because the DSP math is
  unclear (it isn't) but because it's the one mode touching KICKR's layer-mixing
  architecture rather than staying inside the body oscillator — already flagged in
  the improvement doc as needing extra design attention.

## Recommended Solution
Implement all 8 modes as described above. Suggested grouping to minimize duplicated
code in `KickVoice`:
1. **Bend/Skew** — existing code, untouched.
2. **Sync** — phase-reset with amount→ratio mapping; start without PolyBLEP (rely on
   existing oversampling), add PolyBLEP only if the existing morph stress-test
   framework (`RunTests.cpp`) shows aliasing artifacts at 1x/2x oversampling.
3. **Fold** — `y = sin(gain * x)`, amount→gain mapping, tune ceiling by ear.
4. **PD** — piecewise-linear phasor remap through the body osc's existing sine
   lookup/generation path, applied continuously (not attack-only like Bend/Skew).
5. **FM / AM / RM** — share one internal fixed-ratio pitch-tracked sine modulator;
   FM uses it for phase modulation, AM/RM differ only in unipolar-shift-before-multiply
   (AM) vs. bipolar multiply (RM).
6. **FM-from-Sample** — same FM math as (5), modulator signal swapped for the Sample
   layer's live output (normalized/clamped), with graceful degrade-to-0 when the
   Sample layer is empty/disabled.

## References
- [Phase distortion synthesis (Wikipedia)](https://en.wikipedia.org/wiki/Phase_distortion_synthesis)
- [Phase Distortion Synthesis (electricdruid.net)](https://electricdruid.net/phase-distortion-synthesis/)
- [Understanding CZ algorithms (Gearspace)](https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1327653-understanding-cz-algorithms.html)
- [Wavefolding (ringbuffer.org)](https://ringbuffer.org/sound_synthesis_introduction/Distortion/wavefolding/)
- [PolyBLEP Hard Sync Sawtooth Oscillator (KVR DSP forum)](https://www.kvraudio.com/forum/viewtopic.php?p=5788101)
- [Hard Sync Without Aliasing (ResearchGate)](https://www.researchgate.net/publication/2406328_Hard_Sync_Without_Aliasing)
- [Antialiasing Oscillators in Subtractive Synthesis (ResearchGate)](https://www.researchgate.net/publication/3321831_Antialiasing_Oscillators_in_Subtractive_Synthesis)
- [Ring modulation (Wikipedia)](https://en.wikipedia.org/wiki/Ring_modulation)
- [Synthesis Chapter Four: Audio-rate AM and RM (cmtext.com)](https://cmtext.com/synthesis/chapter4_am_rm.php)
- [What's the difference between amp-mod and ring-mod? (Gearspace)](https://gearspace.com/board/audio-student-engineering-production-question-zone/907638-whats-difference-between-amp-mod-ring-mod.html)
- [FM Kick / Bass (dadamachines docs)](https://docs.dadamachines.com/tbd-16/machines/fm-kick)
- [KICK DRUM WORKSHOP: FM IT (Hobotech)](https://hobo-tech.com/technologies/livetips/kick-drum-workshop-fm-it/)
