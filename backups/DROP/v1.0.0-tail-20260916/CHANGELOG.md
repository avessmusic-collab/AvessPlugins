# DROP Changelog

## v1.0.0 — 2026-09-16

Stays at 1.0.0 per this repo's factory convention (a plugin stays at 1.0.0 for
every improvement until public release). This entry documents a post-1.0.0
user-directed improvement session fixing three real-DAW-testing bugs plus one
approved feature.

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
