# DROP Changelog

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
