# DROP Changelog

### Changed — 2026-09-19 (Build Length default 4 → 16 bars)

Stays at 1.0.0 per this repo's factory convention. User request: "make
default 16 bar selected."

- **`buildLength` default index 2 (4 bars) → index 4 (16 bars)** — a
  fresh instance arms a 16-bar build. Choice list/indices unchanged;
  saved sessions carry their own value; factory presets keep their own
  authored lengths. UI's statically-active step circle moved 4 → 16 in
  lockstep.

### Fixed — 2026-09-19 (bug-check round: idle-transparency leak + dropdown edge cases)

Stays at 1.0.0 per this repo's factory convention. User request: "check
for bugs." Code-review-driven audit of the recent rounds; one significant
DSP bug and two UI edge cases found and fixed.

- **Idle-transparency leak (significant, fixed):** an UNTRIGGERED
  instance colored the signal at only ~14dB below the input at 100% wet
  (~-20dB at the 50% default) — clearly audible before DROP was ever
  pressed. **Root cause:** the Style curves author small nonzero START
  weights (budget envelope ~0.05, module-curve starts up to ~0.1), and
  `computeWeights()` at progress 0 handed every module that small live
  weight during Idle/Arming too — correct DURING a build, wrong outside
  one. **Fix:** all shared-pool weights are hard-zeroed while
  Idle/Arming; Building entry steps from 0 to the tiny authored starts
  under each module's ~20ms weight smoother exactly at the bar boundary.
  Side benefits: delay/reverb buffers stay unfed while idle (no stale
  pre-trigger echoes) and the post-cut return to Idle stays bit-dry.
  Measured: idle residual now ~-138dB (float noise floor), was -14dB.
  Peak/Impact/Recovery weights untouched (transient-preservation
  calibration preserved — full suite passes unchanged).
- **UI: choice dropdown edge cases:** switching MAIN/ADVANCED pages left
  an open dropdown floating over the other page (its anchor hidden), and
  a window resize left it at a stale position/scale. Both now close the
  menu.
- Tests: new idle-transparency block (locks the fix at 60dB minimum,
  fails the pre-fix leak by construction). 261 assertions, all passing.

### Changed — 2026-09-19 (Vacuum volume fades are now decibel-linear)

Stays at 1.0.0 per this repo's factory convention. User request: "make
the vacume volume in db."

- **VacuumModule's collapse and return fades are now dB-LINEAR** —
  previously raised-cosine fades in AMPLITUDE, which measured in decibels
  hang near full level and then plunge (the same perceptual class as the
  superseded tail-envelope and noise-rise shapes, both already converted
  to dB-linear). Equal slices of the vacuum's fade now drop/raise equal
  dB between 0dB and the depth-mapped floor (−12dB..−36dB, which was
  always specified in dB) — a perceptually even dive into the vacuum and
  return out of it. Hold depth, window timing, fractions, and the
  low/high/width collapse hooks are unchanged.
- Continuity re-verified: value-exact at every seam, worst single-sample
  step 0.159 at the deliberately extreme 120-sample stress window (bound
  0.5), seam slopes at the floor level orders below the click bound. New
  test asserts constant dB slope across the fade-out (spread < 0.5dB per
  decile). 260 assertions, all passing.

### Changed — 2026-09-19 (NOISE curve default 50% → 0%)

Stays at 1.0.0 per this repo's factory convention. User request: "make
the noise curve to be default where the 0% is."

- **`noiseTension` ("Noise Curve" in the DAW, the NOISE chip in the UI)
  default 50% → 0%** — a fresh instance uses the earliest-bloom riser
  shape (exponent k = 2^-1.2 ≈ 0.435, riser audible sooner in the build).
  50% remains the family's neutral point that reproduces the base
  dB-linear curve exactly; parameter ID/range/mapping unchanged, saved
  sessions keep their own value. The Advanced-page graph's gray NOISE
  curve reads the live parameter, so it shows the new default shape
  automatically — graph and sound stay provably identical (the
  curve-honesty test's expectation now tracks the 0% default).
- Tests: default assertion superseded to 0%; curve-honesty expectation
  updated to the param default; neutral-point (0.5) function test kept
  with clarified wording. 256 assertions, all passing.

### Verified/Optimized — 2026-09-18 (full audit: bar precision at every BPM, knob correctness, perf)

Stays at 1.0.0 per this repo's factory convention. User request: "check
for bugs and optimise everything, check so knobs work corectly. so the
ammount of bars the soun plays is perfect at every bpm."

- **Bar precision at every BPM — verified, no bug found.** New end-to-end
  grid tests deliberately use awkward conditions: fractional bar lengths
  (174 BPM → 66206.9 samples/bar; 140 BPM in 3/4 → 61714.3), 44.1kHz with
  a 441-sample block, decimal BPM (97.3, the live-tapped-tempo case), and
  a 3/4 meter. In every case Impact lands ON the host's bar-counted
  marker (measured deltas are pure block-observation granularity; 90 BPM
  case delta exactly 0.0) and the audible riser ends BEFORE the marker.
- **Knob correctness — verified.** All 16 UI knob defaults/ranges
  cross-checked against the APVTS layout (exact match). New end-to-end
  proofs for the two previously-unproven global stages: DRY/WET at 0% is
  transparent to within float epsilon (5.96e-08 max deviation) even
  mid-build with the riser at full macro, and OUTPUT at −20dB scales the
  final signal by exactly 0.1000. Module macros were already covered by
  the existing weight-sweep/transparency blocks.
- **Optimization:** the SHEPARD riser recomputed its 6 partial
  frequencies + windows EVERY SAMPLE (6× pow + 6× sin + 6× floor per
  sample per channel); they depend only on the segment-constant progress
  and are now hoisted per segment — only the 6 oscillators remain in the
  sample loop. Numerically identical output (same expressions, same
  inputs; the bit-determinism test still passes).
- Suite now 256 assertions, all passing.

### Changed — 2026-09-18 (noise default WHITE + choice dropdowns)

Stays at 1.0.0 per this repo's factory convention. User request: "make the
default noise white, everywhere were are multiple choises make a dropdown
menu to choose, keep the arrows too."

- **`noiseType` default STYLE → WHITE** — a fresh instance always risers
  with classic white noise; STYLE (defer to the preset's authored color)
  remains available as an explicit choice. Indices unchanged, so sessions
  saved with any selection reload identically; only sessions predating
  `noiseType` entirely now resolve to WHITE instead of the Style color.
- **Choice dropdowns (chevrons kept):** clicking the Style selector's
  label (Main page) or the NOISE TYPE chip's value (Advanced page) now
  pops a dark-LCD dropdown of every choice; picking one routes through
  the same setter the chevron arrows use, and the arrows are unchanged.
  Build Length's step circles and the trigger-mode pill already display
  all their choices at once, so they stay as-is. One menu at a time,
  closes on outside click, flips above the anchor when near the window
  bottom, scales with the proportional UI scaling.
- Tests: noiseType default assertion superseded to WHITE; the STYLE
  bit-identity test kept (reworded — STYLE is no longer the default).

### Fixed — 2026-09-18 (TRUE CUT: with TAIL at 0% the effect ends AT the downbeat, ms-precise)

Stays at 1.0.0 per this repo's factory convention. User request: "count
the bars with bpm and make it precise to miliseconds. so when the effect
of 8 bars for example ends. when there is no tail and cut is active. so
the sound stops imidietly volumewise."

- **Root cause of the residual lingering:** the bar-counted BPM math was
  already sample-exact (Impact's first sample lands ON the downbeat), but
  TAIL=CUT only shortened RECOVERY — the Impact STATE still ran its full
  Style-authored window (~220–240ms) past the bar line, exciting the
  dedicated short-tail impact reverb and running the drive overlays, so
  the effect audibly persisted ~a quarter second past the counted bars.
- **Fix:** with CUT active (tail==0%), the ENTIRE post-drop window now
  collapses: Impact state = ~5ms (its only remaining job is handing the
  clean dry return through), Recovery = ~5ms (existing per-sample
  dB-linear fade to exact 0), and the shortTailReverb's send is gated to
  0 during Impact (latched per build, `tailCutActiveForBuild`) so nothing
  is excited at the drop in the first place. Net: the effect's own sound
  is measurably GONE within ~10ms of the BPM-exact bar-counted downbeat —
  measured residual 0.000000 at +10ms (vs 0.23 for tail=0.1%, which keeps
  the full Style-authored impact window and ring byte-identically).
- Tests updated for the collapsed window (Bug F boundary constants +
  residual-based step measurement; per-sample-cut state detection; the
  DAW grid test pins tail above the epsilon since grid timing is
  tail-independent). 250 assertions, all passing.

### Verified — 2026-09-18 (16-bar grid alignment investigated, engine proven exact)

User DAW report: "the 16 bar riser doesnt end in time with the ableton 16
bar markers. the sound persists and stays." Investigation added the
previously-missing END-TO-END grid test: a real advancing host playhead
(ppq moving every block, Ableton-style), trigger applied host-style at the
start of the buffer containing bar 4's line, 16-bar build, riser isolated.
Measured: Impact lands ON the 16-bar marker (block-granularity delta only)
and the audible riser ends BEFORE the marker (the Vacuum's 1/32-bar
silence precedes the drop). The engine is sample-accurate; no code change.

Likely causes of the reported symptom (host-side):
1. **Build Length automation lanes recorded before the 32-bar addition
   (2026-09-17)**: choice-parameter automation is stored NORMALIZED by
   hosts, so a lane point at the old top-of-range "16" (1.0) now selects
   **32 bars** — the riser then runs a full extra 16-bar phrase ("persists
   and stays"). Fix in the session: re-touch/redraw those points (or
   re-click 16 in the UI). The UI's lit step circle during a build shows
   which length is actually latched.
2. A trigger automation point sitting slightly AFTER the bar line arms to
   the NEXT bar (+1 bar late by design — the drop still grid-locks).
3. A stale plugin binary in the DAW cache (rescan/re-add after installs).

- Tests: the new "[POST-1.0] DAW grid lock, end-to-end" block stays as a
  permanent regression guard (250 assertions total, all passing).

### Added — 2026-09-18 (riser source choices + Dry/Wet default 50%)

Stays at 1.0.0 per this repo's factory convention. User request: "make
default dry wet 50%, add choices for different kinds of noise, search what
noises and sounds are used to make risers."

- **New `noiseType` parameter (22nd, strictly additive)** — selects the
  riser's sound source, grounded in the standard riser sound-design
  palette (researched 2026-09-18: white-noise sweeps are the classic EDM
  riser; pink/brown colors read darker/warmer; resonant filter sweeps;
  tonal saw risers climbing an octave-plus add melodic direction; Shepard
  tones give the endless-rise illusion):
  - **STYLE** (default) — the active Style's authored color, exact legacy
    behavior; old sessions load bit-identical.
  - **WHITE / PINK** — classic noise colors.
  - **BROWN** — deepest 1/f² "rumble" build (leaky-integrated white).
  - **SWEEP** — white noise through a resonant low-pass (TPT SVF, Q 2.5)
    swept exponentially 200Hz→12kHz with build progress.
  - **SAW** — tonal polyBLEP saw climbing 4 octaves (A1 55Hz → A5 880Hz)
    across the build, right channel +0.6% detuned for width.
  - **SHEPARD** — 6 octave-spaced sin²-windowed sines over 55Hz–3.5kHz,
    each rising one octave per build with wrap (endless-rise illusion).
  Every source runs through the SAME rise envelope (the gray graph curve
  stays honest for all of them), and RNG consumption is type-independent
  (`nextPink` refactored to consume the caller's white draw), so chaos
  reproducibility and offline-render determinism hold under `noiseType`
  automation. UI: new NOISE TYPE mini-LCD chevron chip (Advanced page,
  below the graph, left of SYNC).
- **Dry/Wet default 100% → 50%**, and all 16 factory presets re-authored
  to 50% in lockstep (selecting a preset never snaps the mix back to
  full-wet). Saved sessions keep their own value.
- Fixed in passing: the UI's `BUILD_LENGTH_BARS` JS array was missing the
  32 added 2026-09-17 (currently write-only, so no visible bug).
- Tests: noiseType contract + per-type render block (each source exactly
  silent at progress 0, audible late, bit-deterministic; STYLE default
  bit-identical to legacy), dryWet/preset 50% assertions; the two
  21-parameter count assertions superseded to 22.

### Changed — 2026-09-17 (synthesized impact hits removed factory-wide)

Stays at 1.0.0 per this repo's factory convention. User request: "remove
impacts" — the follow-up to the same-day Dark Techno report ("it adds a
drum kick or a hit"), which had disabled the synthesized accents for that
one Style.

- **`DropStyleData::impactAccentLevel` now DEFAULTS to 0.0** — no factory
  Style layers ImpactEngine's synthesized 60Hz sub-hit ("kick") or 2.2kHz
  noise burst ("snap") on the drop any more. Every drop is the source's
  own clean return, per the spec's impact philosophy (the accents were
  always "optional").
- The `impact` parameter/knob is UNCHANGED and still fully functional: it
  continues to govern the Vacuum depth, width expansion, saturation
  overlay, and the short-tail impact reverb. Only the additively
  synthesized hits are gone.
- The mechanism (and ImpactEngine's generators) are kept intact and
  per-Style overridable, should accents ever be wanted back as an option.
- Tests: the accent block now asserts ALL 16 Styles author 0.0
  (supersedes the Dark-Techno-only assertions from earlier the same day);
  the engine-level checks (accentLevel 0 adds exactly nothing / 1 renders)
  stay as the gate's behavioral contract.

### Added — 2026-09-17 (32-bar Build Length choice)

Stays at 1.0.0 per this repo's factory convention. User request: "ad a 32
bar choice."

- **Build Length gains a 6th choice, 32 bars** — appended after 16 so the
  original five keep their exact indices: saved sessions (which store the
  actual choice index) reload identically. Only normalized host-automation
  lanes recorded on Build Length remap at the top end (a lane pinned at 1.0
  now means 32 bars instead of 16 — re-touch those points if you had any).
- Engine: `kBuildLengthBars` mapping extended `{1,2,4,8,16,32}`; the
  index clamp raised accordingly. Everything downstream (MusicalClock
  bar→sample conversion, vacuum carve-out, grid alignment) is
  length-agnostic and needed no changes.
- UI (Main page): 6th step circle "32"; step-row widened 15.75rem →
  19.0625rem (still clears the center column — see v10-ui.yaml
  `build_length_32_bars`). `index.html`/`v10-ui.html` kept byte-identical.
- Tests: new `[POST-1.0]` block — choice-list contract (6 entries, original
  indices unchanged) plus an end-to-end render at index 5 proving Building
  genuinely lasts ~32 bars (the old clamp to index 4 fails it by
  construction); one existing test's normalized set-call updated for the
  6-choice mapping (same intended index).

### Fixed — 2026-09-17 (per-sample tail cut, TAIL default 0%, knob rows moved left)

Stays at 1.0.0 per this repo's factory convention. User DAW report: "move
all the knobs to the left add the tail knob lower to the knob rows. fix the
sound. It doesnt cut immedietly. it has artifacts in the end. make the
default tail zero."

- **"Doesn't cut immediately / artifacts in the end" — segment-granularity
  bug in the tail envelope.** **Root cause:** the (correct) dB-linear tail
  envelope was evaluated ONCE per segment at the segment's start and applied
  as a constant. Segments are cut only at state boundaries, so at `tail=0%`
  the entire ~5ms `kTailCutSeconds` Recovery window sat inside ONE segment
  with envelope `env(0) == 1.0` — the fade literally never ran, and the
  Recovery→Idle transition then hard-muted the still-full-level
  delay/reverb/impact tails in a single sample (the audible end click). At
  larger host block sizes even long tails staircased (up to ~-8dB
  instantaneous steps per 1024-sample block on the 150ms minimum tail).
  **Fix (single-application rule):** the envelope math is unchanged
  (`tailEnvelopeForT()`, dB-linear 60dB + raised-cosine landing to exact 0)
  but is no longer multiplied into any per-segment weight — the frozen
  Delay/Reverb returns and ImpactEngine run unscaled through Recovery, and
  one per-sample blend at the end of `processSegment()`
  (`out = dry + (wet − dry) × env(t_sample)`, against the existing
  `dryCaptureBuffer`) applies the decay at audio rate, reaching an EXACT 0
  on Recovery's final sample regardless of block size. Every wet stage it
  replaces was linear in its return weight, so this equals the intended
  per-sample product.
- **`tail` default 35% → 0% (TAIL=CUT)** at user direction — a fresh
  instance cuts cleanly at the drop; ring-out is opt-in. Parameter ID,
  range, and mapping unchanged (strictly a default change; saved sessions
  are unaffected — they carry their own value).
- **UI (Advanced page):** the 2×5 module-macro grid moved LEFT by 8rem
  (first column now aligns with the TENSION/CHAOS chips at left:2.5rem);
  the TAIL knob joined the LOWER macro row as its 6th knob (same pitch),
  vacating its below-the-graph spot; its static label now reads
  "TAIL · CUT" matching the 0% default. `Source/ui/public/index.html` and
  `.ideas/mockups/v10-ui.html` kept byte-identical; see v10-ui.yaml's
  `knob_rows_left_shift_and_tail_join` amendment.
- **Tests:** new end-to-end per-sample-cut block (1024-sample blocks —
  larger than the whole cut window, reproducing the pre-fix failure by
  construction: charged tails, silent input, asserts no single-sample step
  across the cut and true silence after Idle); the two 35%-default
  assertions superseded to 0%.

### Fixed — 2026-09-17 (Dark Techno: unwanted synthesized "kick" at the drop)

Stays at 1.0.0 per this repo's factory convention. User DAW report: "on the
dark techno preset somehow it adds a drum kick or a hit, why? explain and
remove it."

- **Root cause:** `ImpactEngine` intentionally layers two synthesized
  accents on top of the source's return at the drop — a 60Hz sine "sub-hit"
  with a ~1.5ms attack / ~120ms decay (i.e. exactly the envelope of a kick
  drum) and a 20ms band-passed 2.2kHz noise "snap" — scaled only by the
  Impact knob and applied unconditionally for EVERY Style. The Dark Techno
  preset ships with Impact 65%, and techno source material already carries
  its own kick on the very downbeat the drop lands on, so the accent reads
  as a foreign added drum hit rather than reinforcement. (The
  implementation-spec's impact philosophy calls these accents *optional*:
  "clean return + optional short sub/noise accents".)
- **Fix (data-driven, per the no-special-cases rule):** new
  `DropStyleData::impactAccentLevel` (default 1.0 — behavior of every other
  Style is bit-identical) scales ONLY the synthesized sub-hit + burst;
  Dark Techno authors it to 0.0. The source-transient emphasis, width
  expansion, short-tail impact reverb, Vacuum depth, and low-end safety
  remain fully governed by the `impact` parameter and are unchanged.
  `ImpactEngine::process()` gained a trailing `accentLevel01 = 1.0f`
  parameter (existing call sites unaffected).
- **Tests:** new `[POST-1.0]` block — asserts Dark Techno authors 0.0 and
  Clean EDM keeps 1.0, and renders the impact window on a silent bus
  proving accentLevel 0 adds EXACTLY nothing while accentLevel 1 on the
  identical input does render the accents.

### Fixed — 2026-09-17 (bug-audit round: noise curve honesty/level, natural tail decay)

Stays at 1.0.0 per this repo's factory convention. User-directed bug-audit
session, same day as the sessions below: "check the app for buggs and so
everything works smooth. so the sound rises gradualy. and doesnt have
artifacts so the tails are natural reverb delay. and so the sound isnt cut
in an instant when the tail is up. the white noise rises too fast and too
strong, the curve shows one thing the white noise does more than it shows."

- **"The curve shows one thing, the white noise does more than it shows"
  (compounded-progress-curves bug, 4th instance of this bug class).**
  **Root cause:** `IntensityBudget::computeWeights()`'s Noise slot is itself
  a Style-authored, progress-shaped curve
  (`style.moduleCurves[Noise].evaluate(progress) * macro * sourceScalar`),
  and `processSegment()` fed that weight straight into
  `NoiseModule::process()`, which INTERNALLY multiplies it by its own
  `riseAmountForProgress(progress)` — two independently progress-dependent
  gains compounding, while the Advanced-page graph only ever plots
  `riseAmountForProgress()` alone. **Fix:** new
  `drop::IntensityBudget::computeNoiseWeight()` (no `progress` parameter at
  all) is now the SOLE source of the audible noise weight and the published
  viz glow (`activeModuleWeights[Noise]`) — it samples the Style's Noise
  curve once at its END (progress==1.0, a per-style ceiling, not a live
  progression), keeping each Style's authored flavour while ensuring
  progression lives ONLY in `riseAmountForProgress()`. `computeWeights()`'s
  own Noise slot is unchanged/still computed internally (10-slot
  array/UI-contract preserved — mirrors how `impact` is deliberately outside
  the shared budget pool) — it's simply no longer read for the audible noise
  or the viz glow. **No UI change is required** — the graph already plots
  `riseAmountForProgress()`, so it is now provably truthful about what's
  audible, rather than needing to change to match a different reality.
- **"The white noise rises too fast and too strong" (level).** New
  `NoiseModule::kBaseLevel` (0.5, ~6dB) flatly reduces the riser's overall
  output level, independent of the curve-honesty fix above (which fixes the
  SHAPE; this fixes the raw LOUDNESS).
- **"So the sound isnt cut in an instant when the tail is up" (natural tail
  decay).** **Root cause:** the Recovery tail envelope was `(1-t)^2` — a
  shape that, measured in decibels, ACCELERATES toward the end (-5dB at
  t=0.25, -12dB at t=0.5, -24dB at t=0.75, then plunges to -inf), so a long
  tail rang audibly and then perceptibly SLAMMED shut near Recovery's end.
  Natural reverb/delay decay is exponential — i.e. CONSTANT dB slope over
  time. **Fix:** the tail envelope is now genuinely decibel-linear
  (`kTailFadeRangeDb` = 60dB total range: -15dB at t=0.25, -30dB at t=0.5,
  -45dB at t=0.75 — a steady, natural fade instead of an accelerating one),
  with a smooth raised-cosine landing to an exact 0.0 over only the final
  ~3% of the window (`kTailLandingStartT` = 0.97) so the click-free
  Recovery→Idle module-hygiene `reset()` invariant still holds exactly (by
  t=0.97 the level is already ~-58dB, deep in the noise floor, so the forced
  landing is inaudible).
- **Audit findings (no code change):** confirmed the frozen
  `DelayModule`/`ReverbModule` Recovery-tail return weights are (by design,
  unchanged) captured at Building's OWN final value — the loudest point of
  the build — which is correct/intentional (the tail should start exactly
  where the source content left off); with the noise fix above taming the
  overall energy feeding those sends during Building, no additional return
  scaling was found to be warranted. Also found and fixed one stale doc
  comment in `processSegment()` (Processing Order step 4/5 description)
  that still described Noise as inserted "after Digital, before Reverb" —
  a leftover from before the 2026-09-17 "NOISE FIRST" head-of-chain change;
  corrected to reference the current (head-of-chain) position.

Tests: 216 pre-existing assertions preserved, with ONE documented
supersession (the "[POST-1.0] Tail envelope lowers the volume progressively"
block, which asserted the now-superseded `(1-t)^2` shape's own values —
replaced with dB-linear expectations and full rationale). 3 new
`[POST-1.0]` test blocks added: tail-envelope dB-linearity/milestones/
C0-continuous-landing, noise-weight progression-independence (contrasted
directly against `computeWeights()`'s own still-progress-coupled Noise slot,
proving the fix addresses a real bug, not a vacuous property), and an
end-to-end "curve honesty" test that renders a full silent-input/noise-only
build and asserts the measured RMS trajectory matches
`riseAmountForProgress()` within ±0.1 absolute at every decile from t=0.3
upward — the literal "graph tells the truth" guard the user asked for. See
`Source/Tests/RunTests.cpp`.

### Added / Changed — 2026-09-17 (noiseTension parameter + TAIL knob relocation)

Stays at 1.0.0 per this repo's factory convention. User-directed session, same
day as the "gradual noise rise" session below: "move tail knob below the graph
and add a box near curve, white noise curve" — clarified as "box near the
tension box for white noise tension to control the white noise graph."

- **Added:** `noiseTension`, the 21st APVTS parameter (Float 0–100%, default
  50%, strictly additive — parameter IDs 1–20 unchanged). Shapes HOW
  `NoiseModule`'s white-noise riser builds (same directional semantics as
  `tension`: higher = later bloom, lower = earlier bloom) — does **not**
  change the noise LEVEL (still the `noise` macro's job). Implementation:
  `drop::NoiseModule::riseAmountForProgress()` gained a second argument
  (`noiseTension01`, default 0.5) implementing `pow(smoothstep(progress), k)`
  with `k = pow(2.0, (noiseTension01 - 0.5) * 2.4)` — `k == 1.0` exactly at
  the 50% default, so the new default reproduces the pre-`noiseTension`
  smoothstep bit-for-bit (verified by test). A session saved before this
  parameter existed (APVTS's standard missing-node → constructor-default
  behavior) loads and sounds byte-identical to the pre-`noiseTension` build.
- **Added (UI):** a new "NOISE TENSION" dark-LCD chip, same family/
  interaction as TENSION/CHAOS (drag + chevrons + gesture bracketing),
  placed directly beneath them — in the spot the TAIL knob vacated (see
  below), reading as "near the tension box" while also avoiding an orphaned
  gap. The Advanced page's gray NOISE curve now redraws live as this chip is
  dragged, using the exact same shared `riseAmountForProgress()` math the DSP
  runs.
- **Changed (UI):** the TAIL knob moved from the Advanced page's left column
  (below Tension/Chaos) to a new spot directly below the tension-curve graph,
  left of the SYNC chip — per the user's explicit request. Its visual
  treatment, "TAIL · NN%"/"TAIL · CUT" caption, and all existing behavior are
  unchanged, only its position moved.

Tests: all 194 pre-existing assertions preserved (no supersessions — nothing
prior encoded the fixed-shape noise rise's exact numeric output at a
non-default noiseTension, since noiseTension didn't exist yet). 5 new
`[POST-1.0]` test blocks added: default-reproduces-smoothstep-exactly,
monotonic+endpoints across 5 noiseTension settings, directionality
(higher noiseTension ⇒ later bloom), 21-parameter count + default, and
pre-noiseTension (20-param) saved-session compatibility. See
`Source/Tests/RunTests.cpp`.

### Changed / Removed — 2026-09-17 (gradual noise rise / MIDI note selection removed / dual tension-noise curve)

Stays at 1.0.0 per this repo's factory convention. User-directed session, four
related changes from one request: "make so the noise adds gradualy, remove
midi note selection and add a second curve to the graph make the main purple
and make the second gray the second curve will be the noise, how it gradualy
adds."

- **Changed:** `NoiseModule`'s riser used `progress * progress` (quadratic) -
  inaudible until very late in the build, then arriving abruptly (the same
  class of complaint as the earlier "curve progression" bug). Replaced with a
  new, pure, shared `drop::NoiseModule::riseAmountForProgress()` (smoothstep,
  `p*p*(3-2p)`) - monotonic, genuinely C1-continuous at both endpoints, clearly
  audible by 20% progress (0.104 vs. the old shape's 0.04) and well underway
  by 50% (0.5 vs. 0.25). Exposed as a `static` function specifically so the
  UI's new second curve (see below) is provably the same math the DSP uses -
  no duplicated formula that could silently drift. The brightness sweep was
  left unchanged (a plain linear map, never quadratic, so it didn't share the
  "abrupt" character).
- **Removed:** the "MIDI NOTE" mini-screen chip (`#midiNoteChip`, added
  2026-09-15) and its `setMidiTriggerNote` native-function bridge, from
  `Source/PluginEditor.{h,cpp}` and `Source/ui/public/index.html` (+ the
  byte-identical `.ideas/mockups/v10-ui.html`). **Design decision:** with no
  UI element left to pick a note, MIDI trigger mode now responds to **ANY**
  note-on message - most useful for triggering from a MIDI clip or a
  controller pad. The underlying `midiTriggerNote` atomic and its persisted
  `<DropCustomState midiNote="...">` field are **kept**, unused by trigger
  detection now, purely so a session saved before this change still loads
  without error.
- **Added:** a second curve on the Advanced page's TENSION CURVE display,
  showing `NoiseModule::riseAmountForProgress()`'s shape (evaluated at the
  same curved-progress basis `NoiseModule::process()` itself receives) - "what
  the user will actually hear the riser do across the build," on the same
  linear x-axis as the existing tension curve. Small "— TENSION" / "— NOISE"
  inline legend labels distinguish the two, in the existing LCD text style.
- **Changed (colors):** the main (tension) curve is now **purple**
  (`#A78BFA`); the second (noise) curve is **gray** (`#8A8F98`, reused from
  the tension curve's own prior color). The rest of the UI's orange accent
  (`#E8823C`) is unchanged.

Tests: all 186 pre-existing assertions preserved (no supersessions of noise-
rise-curve assertions were needed - the old `progress*progress` shape was
never itself directly asserted on). One pre-existing MIDI test assertion
("non-matching MIDI note does NOT trigger") is a DOCUMENTED SUPERSESSION - it
encoded the now-removed note-matching behavior; replaced with an assertion
that an arbitrary note-on DOES trigger. Plus 3 new `[POST-1.0]` test blocks:
noise-rise gradual/monotonic/continuity + audible-early-rise floors (proven to
fail against the old quadratic shape "by construction"), MIDI-any-note
trigger/non-trigger coverage, and saved-session backward compatibility for the
old `midiNote` field. See `Source/Tests/RunTests.cpp`.

### Fixed — 2026-09-16 (grid lock / transport-stop safety / TAIL=CUT)

Stays at 1.0.0 per this repo's factory convention. Second real-DAW-testing
session the same day: three new user-reported bugs plus one approved
feature, independent of (and layered on top of) the earlier "curve
progression" and "tail envelope" sessions below.

- **"It's not locking to the grid at all — as if it's using its own
  clock."** **Root cause:** `computeDurationsForNewBuild()` quantized Arming
  to the next bar boundary and set `buildingSamples` to the FULL requested
  bar-length duration, then let Peak's Vacuum-collapse window run AFTER
  that — so Impact (the actual "drop") always landed `peakSamples` samples
  LATE relative to the downbeat the user was building toward (>= the
  Style's `vacuumFloorSeconds` floor, 25ms for the default Style, more at
  slow tempi/long vacuum divisions). **Fix:** the Vacuum window is now
  computed FIRST and carved OUT of the requested bar-length duration
  (`buildingSamples = barsToSamples(bars) - peakSamples`, floor-clamped to
  >= 1 sample so Building can never become zero/negative-length), so
  `armingToBuildingSamples + buildingSamples + peakSamples` sums to EXACTLY
  the quantized-start-point + the requested bar count — Impact's first
  sample now lands precisely on the downbeat. See
  `computeDurationsForNewBuild()`'s doc comment in `Source/PluginProcessor.cpp`.
- **"There are artifacts then the sound stops."** **Root cause:** nothing in
  the codebase ever read `AudioPlayHead::PositionInfo::isPlaying` — a host
  transport STOP mid-transition (e.g. the user hits stop mid-build) left the
  entire state machine (noise riser still rising, gate still chopping,
  delay/reverb tails still ringing) running against silence/discontinuous
  audio, audible as chattering artifacts that only trailed off once
  Recovery's own tail eventually finished (potentially seconds later).
  **Fix:** `MusicalClock` now exposes `isHostPlaying()`; `processBlock()`
  detects "a transition is active AND a valid host playhead reports
  `isPlaying == false`" and gracefully ABORTS — a short (~5ms), click-free
  raised-cosine fade of whatever wet/generated content was already playing,
  converging to the same pre-chain dry capture the final Dry/Wet stage
  already uses, followed by forcing the state machine to Idle and running
  the identical module-hygiene `reset()` sweep used at the natural
  Recovery→Idle transition. Deliberately gated on a VALID host playhead —
  the Standalone app / bare-`AudioProcessor` fallback transport has no
  concept of "stopped" and remains fully usable; a new build also cannot be
  armed while a valid host playhead reports stopped.
- **"Fix it so the sound cuts clean, or give me a choice to continue the
  tails."** User chose: TAIL knob at 0% = clean cut. **Fix:** `tail == 0%`
  (within a small float epsilon) now maps Recovery to a very short (~5ms)
  duration instead of the previous 150ms floor — the existing raised-cosine
  tail envelope then produces a fast, click-free fade-to-silence over that
  short window, audibly an instant clean stop. Every tail value above the
  epsilon is unchanged (same 0.15s–4.0s exponential ring-out mapping, so
  there's no discontinuity in feel just above 0%). UI: the TAIL knob's
  caption reads "TAIL · CUT" at exactly 0% instead of "TAIL · 0%".
- **New:** a small "SYNC" readout on the Advanced page (dark-LCD family
  style) shows the detected tempo and whether a valid host playhead is
  present — "128.0 BPM" when locked to the host, "INT 120" when running on
  the internal fallback transport — so the user can confirm sync at a
  glance. Backed by two new additive `VisualizationFrame` fields (`bpm`,
  `hostPlayheadValid`).

Tests: all 167 pre-existing assertions preserved (not run by this session —
build/test execution is a separate pipeline stage), plus 6 new test blocks
covering grid-lock math invariant + clamp edge case, transport-stop abort +
fallback-transport safety, TAIL=CUT decay/continuity, and BPM publication
(19 new `check()` assertions total). Two pre-existing tests' precomputed
sample offsets were updated with documented rationale (they encoded the old
appended-vacuum timing / the old tail==0%→0.15s mapping, both intentionally
changed by this session's fixes) — see `Source/Tests/RunTests.cpp`'s
"DOCUMENTED SUPERSESSION" comments at each site.

### Fixed — 2026-09-16 (curve progression)

- **Build was inaudible until the final bar** (user DAW report: "I chose 8 bars and the effect
  is active only at the end of the last bar, while the visual on the curve moves"). Two
  compounding causes, both fixed:
  1. **Module-sweep curves were far too end-weighted at the default tension (root cause).**
     Audibility during a build is dominated by each module's internal sweep position (filter
     cutoff, gate rate, pitch amount, noise rise), all driven by `TransitionEngine::evaluate()`.
     At the `tension` default of 50% these measured `Exponential=0.031` and
     `LateAcceleration=0.075` at 25% elapsed — the filter had swept 3–8% two bars into an
     8-bar build and nothing was audible. `LateAcceleration` was the worst offender, reaching
     only 22.5% of its progress across 75% of the build.
     Fix: every shaped curve is now blended toward linear by `TransitionEngine::kCurveBlendDepth`
     (0.6), and LateAcceleration's slow-ramp factor was raised 0.3 → 0.55. New values at default
     tension: Exponential 0.119/0.306/0.592, SCurve 0.168/0.500/0.832, LateAcceleration
     0.183/0.365/0.548 at t=0.25/0.50/0.75. Curve character, relative ordering, and the
     `tension` parameter's meaning are all preserved (verified by test).
  2. **Style weight curves were double-curved.** `IntensityBudget::computeWeights()` received
     the already-curved transition progress and then evaluated the Style's own end-weighted
     curves (EaseInCubic/PeakLate) on top of it. Weight position input is now the linear
     elapsed-build fraction, so any given curve is applied exactly once; the curved value keeps
     driving module-internal sweeps and the UI's tension readout. Seven styles' `budgetEnvelope`
     shapes were retuned EaseInCubic → Linear (endpoints unchanged) to suit the single
     application.
- **Visualization/audio agreement:** `VisualizationFrame::progress` (curve dot + progress strip)
  now uses the same linear basis as the audio weights; `tensionLevel` carries the curved value.

Tests: 167/167. New `[POST-1.0] Module-sweep progression` block asserts every curve type clears
musical early/mid-build floors at default tension (pre-fix Exponential and LateAcceleration
failed all three), that shaped curves stay meaningfully end-weighted vs Linear, and that
`tension` still controls end-weighting. Three Phase 3.1 curve-shape assertions were superseded
with documented rationale (the shapes changed deliberately; properties still asserted).


## v1.0.0 — 2026-09-16

Stays at 1.0.0 per this repo's factory convention (a plugin stays at 1.0.0 for
every improvement until public release). This entry documents a post-1.0.0
user-directed improvement session fixing three real-DAW-testing bugs plus one
approved feature.

### Fixed (fourth pass — "curve progression" bug, same session)

- **"I chose 8 bars and the effect is active only at the end of the last bar,
  while the visual dot on the curve moves."** — real-DAW-testing report.
  **Root cause:** stacked end-weighted easing. `PluginProcessor::
  computeIntensityProgress()`'s `Building` case returned
  `transitionEngine.evaluate(t)` — the CURVED (already end-weighted:
  SCurve/Exponential/LateAcceleration all bias "most tension near the end")
  progress. `PluginProcessor::processSegment()` then fed that SAME curved
  value into `IntensityBudget::computeWeights()`, which evaluates it against
  the active Style's OWN authored `moduleCurves[i]`/`budgetEnvelope` — most of
  which (`EaseInCubic`, `PeakLate`) are ALSO end-weighted shapes
  (`Source/Core/DropStyle.cpp`). Two end-weighted curves composed together
  compress virtually all audible weight into the last ~10-15% of the build —
  at 8 bars, effectively "only the last bar."
  **Fix:** single-curve-application architecture. `computeIntensityProgress()`
  (CURVED) is now consumed ONLY by (a) each fixed-series DSP module's own
  internal-sweep `progress` argument (filter cutoff sweep, gate/stutter
  step-length interpolation, pitch-ramp shape, delay feedback ramp,
  saturation drive, digital severity, noise rise/brightness, reverb
  roomSize — all simple linear lerps inside their own modules, verified by
  source inspection, so a single curve application there is musically
  correct and matches `tension`'s documented job), and (b) the new
  `VisualizationFrame::tensionLevel` field ("what the user feels", ramping
  with the shaped curve). A new `computeLinearBuildProgress()` method
  (LINEAR elapsed-build fraction, same Building-only state-gating) now feeds
  `IntensityBudget::computeWeights()` instead — the single point where a
  Style's `moduleCurves`/`budgetEnvelope` shapes are applied, now applied
  exactly once. `VisualizationFrame::progress` (the curve dot's X-position /
  progress-strip fill) switched to this SAME linear basis, so the dot's
  motion now genuinely agrees with the audio weights it represents — the
  user explicitly compared the two. See `PluginProcessor.h`'s
  `computeIntensityProgress()`/`computeLinearBuildProgress()` doc comments
  for the full decision matrix.
- **Style table adjustment.** 7 of the 16 Styles (Clean EDM, Big Room, Dark
  Techno, Cinematic, Vocal Build, Synth Build, Destroy) authored
  `budgetEnvelope` as `EaseInCubic` — reasonable when composed with an
  already-curved input (the pre-fix bug), but still far too end-weighted on
  its own once applied to the linear elapsed fraction directly (verified by
  construction in `RunTests.cpp`'s new weight-progression test). Changed
  shape `EaseInCubic` → `Linear` for these 7 styles' `budgetEnvelope` only
  (start/end values byte-identical, so each style's final `t=1` ceiling —
  and therefore the drop's peak intensity — is unaffected; only the ramp-up
  shape changed). The other 9 styles' `budgetEnvelope` were already
  front-loaded enough (`EaseOutCubic`/`Linear`/`Bump`/`PeakLate`) and were
  left untouched; `Reverse Energy`'s intentionally-declining `Linear`
  envelope (a documented structural exception, not part of this bug) was
  also left untouched. `moduleCurves` (the 10 per-module shapes) were NOT
  changed — the `budgetEnvelope` is the binding constraint (proportional-cap
  normalization) through nearly the entire build for every affected style,
  so it alone governs the audible ramp-up shape.
- **Testing:** 3 new `[POST-1.0]` test blocks in `RunTests.cpp` — weight
  progression milestones (proven to fail the OLD stacked-curve path "by
  construction," using the real project math for both paths), per-curve-type
  coverage (weight-path immunity to `transitionCurveType` post-fix;
  `TransitionEngine`'s own Linear/Exponential/SCurve/LateAcceleration
  relative character preserved for module-internal sweeps), and
  visualization consistency (`VisualizationFrame::progress` linear basis
  matches the audio weights; `::tensionLevel` matches
  `TransitionEngine::evaluate()`). All pre-existing tests (including the
  16-style pairwise-distinctness test and the Phase 3.4
  transient-preservation test) remain green — the `budgetEnvelope` shape
  changes preserve every style's endpoint values and the distinctness
  metric compares full 9-point trajectories across all 10 `moduleCurves`
  (unchanged) plus `budgetEnvelope`, so per-style distinctness is retained
  by a wide margin.

### Fixed (third pass — build/test feedback, same session)

- **Continuity test measured the wrong thing.** The test fed a continuous
  tone straight through Recovery, so windowed RMS rising toward the tone's
  own steady-state level (~0.283) as the tail's wet contribution faded and
  revealed the near-transparent dry bus underneath was correct, expected
  behavior (Recovery is meant to let the live source return cleanly) — not a
  broken decay chain. `processSegment()`'s send/return/tail-envelope wiring
  for `ImpactEngine`/`DelayModule`/`ReverbModule` was re-verified correct by
  direct source inspection. Fixed the test: the input tone now fades to
  silence (raised-cosine, 50ms) ending exactly at Recovery's first sample, so
  the post-boundary measurement genuinely isolates the decaying tail.
- **"State reset" test's residual 0.238242 was a deterministic noise riser,
  not stale state.** `NoiseModule` generates seeded noise independently of
  input by design (reproducibility must be independent of automation
  history), and `ImpactEngine`'s sub/burst are likewise seeded/deterministic
  — both legitimately regenerate identically on every build, including a
  silent-input one (confirmed by the residue matching to 6 decimal places
  across two independent runs — the signature of a reproducible generated
  signal, not decaying leftover state). Fixed the test: `noise` and `impact`
  are now set to 0 before both builds, so any remaining nonzero output can
  only be genuine stale-buffer residue.

### Fixed (second pass — build/test feedback, same session)

- **Transient-preservation regression + rising post-impact RMS.** The first

- **Transient-preservation regression + rising post-impact RMS.** The first
  pass held the new tail envelope at 1.0 through `Peak`+`Impact` (not just
  `Recovery`), substituting `DelayModule`/`ReverbModule`'s RETURN weight with
  the frozen (Building-end) weight during Impact too. Source inspection of
  `IntensityBudget.cpp`/`DropStyle.cpp` confirmed `computeWeights()` at
  `progress==0` was NEVER exactly `0.0f` to begin with (`budgetEnvelope`/
  `moduleCurves` are authored with nonzero `startWeight`, e.g. `0.05`) — the
  substantially larger frozen weight, crossfaded against a near-silent "wet"
  signal (nothing had been send-gated into the buffers yet), diluted the dry
  content (`dry * (1 - returnWeight)`) well below the transient-preservation
  floor. Fixed: Peak and Impact are now byte-identical to the pre-existing
  legacy send/return path (both equal the live `IntensityBudget` weight,
  whatever it genuinely is); only `Recovery` uses the frozen-weight tail
  envelope. This also explains (and the continuity test now correctly
  tolerates) the observed rising-then-falling RMS shape at Recovery's start —
  an intentional ~20ms attack (the return-weight smoother catching up to the
  new, legitimately larger Recovery target) immediately followed by the
  tail envelope's decay, not a bug.
- **Bug C reset still leaking stale content into the next build.** Root
  cause: EVERY DSP module (not just Delay/Reverb/Impact/Stutter) is fed a
  genuinely nonzero live weight even while "ducked" (see above — nonzero
  `startWeight` at `progress==0`), so every module with persistent internal
  filter/oscillator state (`FilterModule`'s resonant `StateVariableTPTFilter`
  in particular) was already live-processing real signal throughout the
  previous build and was never reset. Fixed: the Recovery→Idle buffer-hygiene
  reset now covers all owned DSP modules (Filter/Width/Gate/Saturation/Pitch/
  Digital/Vacuum/Noise, in addition to the original four).
- **Test-design contradiction, "shared weights stay exactly 0 throughout
  Recovery".** Rewritten: `IntensityBudget` weights at `progress==0` were
  never exactly 0 (see above), so this was a false premise unrelated to send/
  return design. Replaced with the true, meaningful, regression-preventing
  property: Recovery's weights never exceed their Peak/Impact ducked
  baseline (the old `1.0f - t` ramp's actual bug was growing substantially
  above that baseline).
- **Continuity test asserted decay from Recovery's very first sample.**
  Corrected to permit the known, intentional ~20ms return-weight-smoother
  attack window before requiring decay, and to assert the RMS peak actually
  falls within that window (ruling out a genuine late-growing artifact).

### Fixed

- **Instant cut at the Impact→Recovery boundary.** `ImpactEngine::process()`
  was state-gated to the `Impact` state ONLY — its sub-hit decay (~120ms) and
  its dedicated short-tail `ReverbModule` instance's own algorithmic decay
  were amputated the moment the state machine advanced into `Recovery`,
  whenever that boundary landed before those envelopes/that reverb had
  genuinely finished ringing.
  **Root cause:** the gating condition `if (dropStateMachine.getState() ==
  drop::DropState::Impact)` in `PluginProcessor::processSegment()` simply
  stopped calling `impactEngine.process()` the instant `Impact` ended — the
  reverb algorithm's internal state was never fed further samples, so it
  couldn't continue decaying; the sub-hit/burst additive content likewise
  vanished mid-envelope.
  **Fix:** `PluginProcessor` now continues calling `ImpactEngine::process()`
  through `Recovery` too, with `elapsed` tracked CONTINUOUSLY across the
  boundary (`impactCarryoverElapsedSamples`, a new member — `Recovery` no
  longer resets it, unlike `DropStateMachine`'s own per-state
  `samplesIntoState`). `ImpactEngine::process()` gained an optional
  `reverbSendWeightOverride` parameter so its dedicated `shortTailReverb`'s
  INPUT can be silenced independently during the `Recovery` carryover (no
  fresh live pass-through audio re-excites the tail) while its OUTPUT keeps
  ringing out, scaled by the same tail envelope described below.

- **Post-impact effect-blast artifact.**
  `PluginProcessor::computeIntensityProgress()` returned `1.0f - t` (ramping
  1→0) for the `Recovery` state, so the 10 shared-budget modules — freshly
  settled to EXACTLY 0 through the entire preceding `Peak`+`Impact` window —
  SLAMMED from 0 back toward full weight at the very first sample of
  `Recovery` (through each module's own ~20ms internal smoother), then
  ramped back down. Audible as a brief but noticeable "blast" of filtering/
  gating/delay/etc. immediately after the drop.
  **Root cause:** `case drop::DropState::Recovery: return 1.0f - t;` combined
  with the `Peak`/`Impact` case's `return 0.0f;` meant the shared-module
  weight input trajectory was `... 0.0, 0.0, [Recovery entry] 1.0, 0.99,
  0.98, ...` — a hard discontinuity at exactly the moment listeners expect
  the impact to settle cleanly, not swell back up.
  **Fix:** `Recovery` now ALSO returns `0.0f` — DOCUMENTED SUPERSESSION of
  the `1.0f - t` ramp — matching `implementation-spec.md`'s own IMPACT
  PHILOSOPHY section verbatim: "BUILD → TENSION → VACUUM → ORIGINAL/CLEAN
  IMPACT → SHORT TAIL → RECOVERY." Recovery now stays clean (shared modules
  stay ducked); only tails ring out during it, via the new send/return-
  weight mechanism below (governed by the new `tail` envelope, NOT this
  shared-budget progress value).

- **Stale delay/reverb tails, muted mid-ring and replayed into the next
  build.** `DelayModule`/`ReverbModule`'s wet mix followed the same
  (now-permanently-0-during-Recovery) shared weight, so their
  already-circulating echoes/reverb tails were muted within one ~20ms
  smoother ramp instead of ringing out — AND neither module's internal state
  was ever reset between builds, so any content still physically present in
  the delay line/reverb buffers at that point would resurface, unrelated to
  the next drop, whenever the read pointer/algorithm happened to revisit it.
  **Root cause:** a single `weight` argument controlled both what fed INTO
  the delay line/reverb algorithm and how much of it was blended to output —
  there was no way to stop feeding fresh (ducked) material into the tail
  while STILL letting the already-excited tail ring out on its own terms.
  Additionally, `DelayModule`/`ReverbModule`/`ImpactEngine`/`StutterModule`
  had no `reset()` call anywhere in `PluginProcessor`'s trigger/state-machine
  lifecycle.
  **Fix:** `DelayModule`/`ReverbModule` gained an additive send/return-weight
  API (`processWithSendReturn()` / `processWithParamsSendReturn()` —
  existing single-weight `process()`/`processWithParams()` unchanged, now
  thin wrappers). During `Peak`/`Impact`/`Recovery`, INPUT-SEND is forced to
  0 (no fresh ducked-bus material re-excites the tail) while WET-RETURN is
  driven by a new raised-cosine "tail envelope" — held flat at 1.0 through
  `Peak`+`Impact`, decaying 1→0 exactly across `Recovery`'s (now
  `tail`-parameter-derived) duration, C0/C1-continuous at both the
  `Impact`→`Recovery` boundary and `Recovery`'s own end — multiplied against
  the LAST weight value `Building` actually used (`frozenDelayReturnWeight`/
  `frozenReverbReturnWeight`), so the tail starts exactly where the source
  content left off. At the click-free instant the tail envelope reaches
  EXACTLY 0 (the `Recovery`→`Idle` transition — detected by the new
  `PluginProcessor::advanceStateMachineTicks()` helper, which supersedes the
  old bare `while (dropStateMachine.tick()) {}` call sites),
  `DelayModule`/`ReverbModule`/`ImpactEngine`/`StutterModule` are all
  `reset()` so nothing stale replays into the NEXT build. Deliberately NOT
  reset at `Idle`→`Arming` (audio is still live at that transition).

### Added

- **`tail` parameter** (20th APVTS parameter, Float 0–100%, default 35%,
  strictly additive — parameter IDs 1–19 unchanged; a 1.0.0 session saved
  before this parameter existed loads with the default). Sets the
  `Recovery` state's duration directly — Recovery IS the tail's ring-out
  window — OVERRIDING the active DropStyle's own `recoveryDivision` timing
  preference (Vacuum/Impact timing remain fully Style-driven). Exponential
  seconds mapping: `tailSeconds = 0.15 * (4.0 / 0.15) ^ (tail/100)` — 0% ≈
  0.15s, 100% ≈ 4.0s, 35% (default) ≈ 0.47s. Latched at Arming, like
  `buildLength`. UI: small neumorphic macro knob (same visual family as the
  10 module macros), "TAIL · NN%" caption, Advanced page left column below
  the Tension/Chaos/MIDI-Note mini-screens.

### Technical Details

- Files modified:
  - `Source/DSP/DelayModule.h/.cpp` — `processWithSendReturn()` additive API
  - `Source/DSP/ReverbModule.h/.cpp` — `processWithSendReturn()` /
    `processWithParamsSendReturn()` additive API
  - `Source/DSP/ImpactEngine.h/.cpp` — `reverbSendWeightOverride` parameter,
    Recovery-carryover support
  - `Source/Core/DropStyle.h` — doc-comment note on `recoveryDivision`'s
    override
  - `Source/PluginProcessor.h/.cpp` — `tail` parameter, tail envelope, frozen
    weight capture, `advanceStateMachineTicks()`, Recovery-carryover
    elapsed-sample tracking, `computeIntensityProgress()` Recovery fix
  - `Source/PluginEditor.h/.cpp` — 20th relay + attachment (`tailRelay` /
    `tailAttachment`)
  - `Source/ui/public/index.html` + `.ideas/mockups/v10-ui.html` — TAIL knob
    markup (kept byte-identical)
  - `Source/Tests/RunTests.cpp` — 5 new `[POST-1.0]` test blocks (20-param
    count, pre-`tail`-session compatibility, `tail`-scales-Recovery-duration,
    zero-weight-throughout-Recovery, post-impact continuity, state-reset-on-
    Idle-entry)
- DSP changes: additive send/return-weight API on `DelayModule`/
  `ReverbModule`; `ImpactEngine` Recovery continuation; new raised-cosine
  tail envelope in `PluginProcessor::processSegment()`; buffer-hygiene reset
  on the Recovery→Idle transition
- Parameter changes: `tail` added as the 20th APVTS parameter (additive,
  IDs 1-19 unchanged)
- Testing: all pre-existing assertions remain green (no supersessions found
  to be strictly necessary — no prior test literally encoded the old
  `1.0f - t` Recovery ramp's exact values); 5 new POST-1.0 test blocks added,
  covering both bugfixes and the new parameter

### Dependencies

- JUCE 8.x
- No new third-party dependencies
