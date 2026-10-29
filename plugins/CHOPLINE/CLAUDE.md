# CHOPLINE — CLAUDE.md

Tempo-synced vocal chop / warp / glitch instrument. The authoritative contract
is `SPEC.md` (user-authored); the locked visual direction is the v17 mockup
(`.ideas/mockups/v17-ui.yaml`, see `.ideas/creative-brief.md` "UI Design — FINAL").
Work phase by phase (spec §11) and STOP after each phase for user approval.

**Current phase: 5 (Pro features) — complete, awaiting approval.**

## Build commands

All commands from `plugins/CHOPLINE/` (self-contained project — NOT built by
the monorepo root CMakeLists.txt):

```bash
# Configure (fetches JUCE 9.0.4 + Catch2 v3.16.1 on first run)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build everything (VST3, AU, Standalone, tests)
cmake --build build --config Release --parallel

# Run unit tests
./build/CHOPLINE_Tests_artefacts/Release/CHOPLINE_Tests
#   (path is ./build/CHOPLINE_Tests_artefacts/CHOPLINE_Tests on single-config
#    generators without per-config subdirs — use whichever exists)

# pluginval (strictness 10, spec §12)
/Applications/pluginval.app/Contents/MacOS/pluginval \
  --strictness-level 10 --validate \
  "build/CHOPLINE_artefacts/Release/VST3/CHOPLINE.vst3"
```

Built artefacts land in `build/CHOPLINE_artefacts/Release/{VST3,AU,Standalone}`.
Do NOT install to system plugin folders until the user asks.

## Decisions made (Phase 0)

- **JUCE 9.0.4** — latest stable JUCE 9 tag at scaffold time (2026-10-24),
  pinned by exact tag via CMake **FetchContent** (chosen over CPM to avoid
  vendoring an extra script; over submodules to keep the monorepo clean).
  The system JUCE 8.0.15 at /Applications/JUCE is NOT used for CHOPLINE.
- **Catch2 v3.16.1** — pinned via FetchContent, test-only.
- **Plugin identity**: `PLUGIN_CODE Chpl`, `PLUGIN_MANUFACTURER_CODE Manu`,
  `COMPANY_NAME "CHOPLINE"` — follows the house de-brand convention (neutral
  placeholder manufacturer code, same as KICKR); the user supplies a real
  company name/code before public release.
- **C++20**, formats VST3 + AU + Standalone, IS_SYNTH, NEEDS_MIDI_INPUT,
  stereo output only (no input buses; sidechain capture bus is reserved for
  Phase 6).
- **Warnings as errors on project sources only** (`-Werror` / `/WX` applied
  per-source-file), because JUCE's CMake compiles JUCE module translation
  units into the same targets and those must not be gated by our -Werror.
- **Namespaces**: `chopline::engine` (DSP, no GUI), `chopline::params` /
  `chopline::state` (APVTS + models), `chopline::ui` (LookAndFeel, views).
- **Test target** (`CHOPLINE_Tests`, juce_add_console_app + Catch2WithMain)
  compiles only engine/state sources — none of our UI/editor code. Note:
  `juce_audio_processors` transitively pulls juce_gui_extra/juce_gui_basics
  as JUCE *module* dependencies; that is JUCE's module graph, not a GUI
  dependency of the engine.
- **State versioning from day one**: `chopline::params::kSchemaVersion` (=1)
  stored as `schemaVersion` on the APVTS root in getStateInformation.
- **Universal binary (arm64 + x86_64)** deferred to release packaging
  (Phase 6): local + CI builds are native-arch for speed. Enable later with
  `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.
- **CI**: `.github/workflows/chopline.yml` (self-contained; monorepo-root
  workflows untouched). macOS + Windows matrix, Release build, Catch2 tests,
  pluginval strictness 10. Follows the house pluginval pattern (see
  prism-windows.yml): on Windows pluginval.exe is a GUI-subsystem binary —
  pipe to force the wait and judge by the printed verdict, not the exit code;
  pwsh masks non-final exit codes, so check `$LASTEXITCODE` after every build
  step.

## Decisions made (Phase 1 — Sampler)

- **SENS is mode-dependent** (locked v17 "lean slicing controls" = one knob):
  Transient → onset sensitivity; Grid → division of the sample BPM
  (1/4·1/8·1/16·1/32 by quartile); Equal → slice count 2–64; Manual → unused.
- **Slicing controls are ValueTree STATE, not APVTS** (v17 scope `slice_map`):
  mode, SENS, min slice length, zero-cross snap, sample BPM and the markers
  live in `SLICES`/`SAMPLE` children of the root. The only Phase 1 APVTS
  parameter is `midiBaseNote` (choice C0–C3, default C1 = 36, Ableton naming).
- **Markers are stored in seconds** (model, message thread); the audio thread
  sees a `SliceMapSnapshot` in sample frames. A session-rate change re-resamples
  in the background and the slice map survives untouched.
- **Snapshot reclamation**: `SnapshotExchange` publishes generation-stamped
  nodes; the audio thread acks the generation at block end; the message thread
  frees a retired snapshot only when a NEWER generation was acked AND no voice
  still reads it (per-stream atomic sample pointers). Consequence: loading a
  new sample never cuts ringing voices — they finish on the old snapshot.
- **Voice stealing**: a retrigger moves the running stream into an in-voice
  2 ms fast-fading TAIL and starts the new note immediately — "steal oldest
  with 2 ms fade" without delaying the new onset. 16 voices preallocated.
- **Declick**: equal-power (sin) fades at chop start/end, per-chop 0–20 ms,
  default 2 ms, applied at playback (never destructive), clamped to half the
  chop length.
- **FLAC blob cached at load** (24-bit, lossless-for-purpose): used for state
  embedding (`embed` toggle in settings, default ON) and as the source for
  session-rate re-resampling (original file read exactly once). Restore
  prefers the embedded blob, falls back to path; path + MD5 hash stored for
  the Phase-later relink dialog (spec §10).
- **Note-off is ignored in Phase 1** (one-shot chop playback). Gate/Loop/Fit
  playback modes and choke groups are Phase 3 per spec §6/§11.
- **Strip A interactions** (per the Phase 1 brief): wheel zoom + scroll,
  draggable markers in ALL modes, double-click add / alt-click delete in
  Manual, click-to-audition, shift-click multi-select. NOTE: the v14/v17 YAML
  says the strip is "exactly v12" with editing only in the chop overlay — the
  overlay sample editor is a later phase; reconcile when it lands.
- **Sample BPM** is detected at load (inter-onset-interval histogram, fallback
  120) and user-editable in the '…' slicing flyout (spec §4 Grid mode).
- **juce_cryptography** linked (MD5 file hash). JUCE 9 writer API:
  `AudioFormatWriterOptions` (the old `createWriterFor` overload is deprecated
  and -Werror fails on it).
- **setStateInformation hops to the message thread** when called off-thread
  (pluginval strictness 10 does this); `getStateInformation` is thread-safe via
  the engine's model lock.
- **Dev-only hooks** (Standalone builds, env vars, no effect in hosts):
  `CHOPLINE_AUTOLOAD_SAMPLE=<path>` loads a file at startup;
  `CHOPLINE_UI_SNAPSHOT=<path.png>` renders the editor to PNG ~6 s after open
  (component snapshot — not a screen capture). Tests: hidden `[.smoke]` case
  uses `CHOPLINE_SMOKE_FILE` / `CHOPLINE_SMOKE_OUT` for a real-vocal offline
  render; never run by default/CI, no user paths committed.
- **Allocation discipline is enforced by test**: `EngineTests.cpp` replaces
  global new/delete in the test binary and asserts zero (de)allocations on the
  calling thread across 1000 processed blocks with triggers (spec §8/§12).

## Decisions made (Phase 2 — Grid sequencer)

- **Pattern model is a ValueTree** (`state/PatternModel`): every edit routes
  through `juce::UndoManager` (100 transactions; gestures group under one
  `beginTransaction`). Events are TIME-BASED — start + length in fractions of
  a bar (locked v17 `events_are_time_based`); changing the note length keeps
  events at their musical positions, off-grid events stay until moved.
- **Note lengths**: 9 values, 1/4 · 1/8 · 1/16 each straight/triplet/dotted
  (NO 1/32 — v17 dropped it). Boxes per bar 4/8/16, 6/12/24, 3/6/11; the last
  dotted box is the fractional bar remainder. Default 1/16.
- **8 pattern slots live in the model and all serialize** (`PATTERNS` child,
  8 `PATTERN` children with `EV` events); the UI surfaces only slot A — the
  slot selector is deliberately deferred to Phase 5 (v17
  `deferred_pattern_slots`), and there is no pattern-slot APVTS param yet.
- **Per-step override FIELDS stored now** (vel 0–127 default 100, pitch st,
  prob %, ratchet): only velocity plays in Phase 2; the rest round-trip
  untouched (unit-tested) until the Phase 5 lane editor.
- **PatternSnapshot** published through the same `SnapshotExchange` as the
  slice map, republished on every model change (model has its own re-entrant
  lock so `getStateInformation` stays thread-safe).
- **Scheduler** (`engine/PatternScheduler`, header-only): position is derived
  from host PPQ every block (`ppq / quartersPerBar`, time sig from the host),
  never an internal counter. Trigger inclusion is tested in ROUNDED SAMPLE
  space (`floor(offset + 0.5)` within `[0, numSamples)`), which keeps triggers
  exact-sample down to block size 1 (bar-space tests clamp boundary triggers a
  sample early). PPQ discontinuity tolerance: `max(0.01q, 25% of the block)`.
- **Voice source tags**: pattern triggers are tagged; transport stop and
  loop/jump discontinuities fast-fade (2 ms) ONLY pattern voices — MIDI and
  audition voices ring on (spec §8). `VoicePool::releaseTagged`.
- **Free-run**: no playhead at all (Standalone) → always free-runs at the
  manual FREE-RUN BPM (settings flyout; stored on the PATTERNS tree, not
  undoable, cached in an atomic for the audio thread). Host present but no
  PPQ → internal accumulator at the host BPM. Accumulator restarts at 0
  whenever the pattern (re)starts.
- **Swing** is the Phase 2 APVTS param (`swing`, 0–75 %, default 0): even
  steps — odd 0-based indices of the CURRENT grid — trigger late by
  swing% × step. Trigger time only, atomic per-block read (no smoothing
  needed); events off the current grid are not swung (by design).
- **Spans are stored and rendered but playback is one-shot** — the chop plays
  from the event start exactly like Phase 1; Gate/span-aware truncation and
  Loop/Fit land in Phase 3 (per spec §6/§11).
- **Bar semantics**: a pattern bar = a HOST bar (time signature honoured for
  bar length); step lengths are fractions of that bar with v17's 4/4 box
  counts. See open questions for odd meters.
- **GridStrip UI**: compact 64×24 note-length button (click cycles
  1/4→1/8→1/16 keeping T/D; 380 ms hold / right-click / right-14 px caret
  zone opens the 9-value menu), pattern-length combo, bar-page seg (only when
  >1 bar; UI-only state), SWING CompactKnob (APVTS attachment). Boxes
  display: ≤12 steps = 1 row, else 2 rows (12×2 max, v17); cards per v17
  tokens, filled = chop-shade tint + index chip + mini waveform (peaksFine) on
  a 12 px grid, selected = 1.5 px blue, playing = pressed grey + inset
  hairline, playhead = 1.5 px blue via the editor VBlank
  (`Engine::getUiPatternPositionBars`, −1 when stopped).
- **Grid interactions**: paint/paint-run with Strip A's selected chop, drag
  from Strip A (editor is the `DragAndDropContainer`; description
  `chopline:chops:i,j,…`; multi-selection fills consecutive cells in index
  order), right-edge drag spans steps, drag moves (replacing the target cell's
  event), Alt-drag duplicates, Delete/Backspace removes, double-click selects
  the chop in Strip A; Cmd/Ctrl-Z / Shift-Cmd-Z on the grid and the editor.
- **Trigger capacities**: 256 events per pattern (snapshot array), ≤64
  pattern triggers per block, 128 merged events (pattern+MIDI+audition) per
  block — all fixed arrays, no audio-thread allocation (detector test runs
  the pattern + swing + stop/start + loop jump over 1000 blocks).
- **Dev hook** (Standalone only): `CHOPLINE_DEMO_PATTERN=1` paints the first
  chops onto the grid ~5 s after open so `CHOPLINE_UI_SNAPSHOT` shows a
  populated pattern.
- **State**: `schemaVersion` stays 1 (PATTERNS is an additive child). A
  Phase 1-shaped state (no PATTERNS child) restores to default patterns —
  unit-tested, as is the full pattern round trip (tree equivalence).

## Decisions made (Phase 3 — Chop FX / warp engine)

- **Per-chop settings live in the slice model, NOT APVTS** (locked v17
  `scope: per_chop`): `engine::ChopSettings` rides on each `SliceMarker`
  (the chop STARTING at the marker) — trims (startOffset/endOffset, seconds),
  fades, loop (start/end normalised + xfade ms), mode, warp, reverse, pitch
  ±24 st, release 5–500 ms, delay/verb sends, unison 1–4, AHDSR, choke group,
  formantPreserve. Serialized as extra properties on the `M` marker children
  (`so eo ls le lx md wp rv pt rl dsn vsn un at hd dc su ck fp`); Phase 1/2
  states (t/fi/fo only) load with defaults — unit-tested. Snapshot `Chop`
  carries everything converted to frames; voices COPY the struct at trigger,
  so a settings edit never glitches a ringing voice (unit-tested).
- **Behaviour change from Phase 2**: default mode is **Gate** (v17) — grid
  spans now truncate playback (release 80 ms) — and ALL chops default to
  **choke group 1** (spec §6 classic vocal-chop monophony). Tests that assert
  polyphony disable choke explicitly (`test::disableChoke`). Choke group 0 =
  off. No choke UI yet (v17 has none) — model field only.
- **Signalsmith Stretch 1.4.0** pinned via FetchContent (MIT; its CMake pins
  signalsmith-linear 0.6.4, also MIT — both in THIRD_PARTY_LICENSES.md).
  `GIT_SUBMODULES ""` skips its cmd/ tooling. Headers included through
  `StretcherPool.h` behind warning-suppression pragmas (-Weverything off for
  the include; our -Werror stays on for our code).
- **Stretcher budget**: 16 DEDICATED stretchers (one per voice — a stretch
  chop can always sound) + 8 SHARED for unison streams 2–4; when the shared
  pool is exhausted extra unison copies are silently dropped (graceful
  degradation). All configured + WARMED in prepare (one outputSeek + process
  at the worst-case rate) so audio-thread reset/seek/process never allocate —
  covered by the allocation-detector test (stretch + unison + choke + all
  modes over 1000 blocks). StretcherPool::prepare early-outs when rate/block
  are unchanged (pluginval calls prepareToPlay repeatedly).
- **Live per-voice stretch, no background pre-render**: spec §6 allows
  pre-render per chop, but Fit's stretch ratio depends on the EVENT SPAN (and
  Gate/Loop interact with live release), so pre-render cannot cover the
  general case; live stretch with `outputSeek()` pre-roll (spec §6 "pre-roll
  inputLatency()") starts chops on time (unit-tested ≤ 2 ms of the trigger
  offset). `splitComputation=true` spreads FFT work across process calls.
  Each trigger of a stretch voice costs a one-off outputSeek burst
  (~outputLatency samples of processing) — profile CPU in the DAW test; the
  Eco switch is Phase 5.
- **Varispeed** = fractional-rate playback with catmull-rom (4-point)
  interpolation; EXACT integer positions take a bit-exact fast path, so
  pitch 0 reproduces Phase 1/2 renders sample-for-sample. Tape-stop/start
  curves (spec §6) deferred — not in v17.
- **Fit step** forces the STRETCH engine regardless of the warp choice
  (pitch preserved by definition); input rate clamped to 0.25×–4× (spec §6
  stretch range): a chop > 4× the span gates at the span end, a chop < 0.25×
  the span ends early. A span-less Fit trigger (plain MIDI note / audition)
  plays as One-shot with the chop's own warp (documented decision).
- **Stretch warp** = time 1:1, pitch independent (v17 has no time knob),
  formant preserve ON by default (`setFormantSemitones (0, true)`,
  `setFormantBase (0)` auto pitch, tonality limit 8000 Hz/rate). The
  formantPreserve flag is engine/state only — v17 surfaces NO formant toggle
  (open question).
- **Unison constants** (documented for taste-tuning): cents spread per count
  {0} / {−7,+7} / {−10,0,+10} / {−12,−4,+4,+12}; stereo positions {0} /
  {∓0.6} / {∓0.7,0} / {∓0.8,∓0.3}; balance-law panning (centre = unity, so
  unison 1 is bit-identical to Phase 2) and 1/√N gain compensation. Unison
  applies in Varispeed (detuned rates) and Stretch (detuned transpose per
  stream, own stretcher).
- **Loop mode**: forward only (spec's ping-pong dropped with v17's lean UI),
  LINEAR crossfade into the pre-loop content (correlated material at the loop
  point — equal-power would bump the level; DC stays flat, unit-tested
  click-free); xfade clamped to half the loop AND the available pre-loop
  audio (loopStart 0 → no crossfade). Keeps looping through the release.
  Reverse+Loop maps the brace into the reversed (logical) domain.
- **Playback/gate semantics**: pattern events gate for their span (computed
  at trigger from the current tempo — a later tempo change doesn't re-scale a
  ringing gate); MIDI notes gate while held (note-offs are sample-accurate —
  merged into the same sorted event list as note-ons — and one note-off
  closes only the OLDEST voice still holding that note, so overlapping
  same-note retriggers keep the newer instance ringing); audition gates for
  one chop's worth. One-shot and Fit ignore note-off (spec §6). AHDSR: linear
  attack/decay/release segments; chop RELEASE = the AHDSR release stage. A
  hard output cut (Fit/stretch end) hides under a 32-sample micro-fade.
- **Steal/choke never allocates or clicks**: a retrigger/steal PRERENDERS the
  old playback's 2 ms fade into a per-voice PCM tail buffer immediately and
  frees its stretchers for the new note (the tail holds no sample/stretcher
  refs); choke/all-notes-off/transport-stop fast-fade the live playback
  (2 ms). Choke fires on any trigger source against every other ringing voice
  in the group.
- **Delay/verb sends are stored + surfaced (overlay knobs) but routed
  nowhere** until the Phase 4 master delay/reverb land.
- **Granular/Freeze**: v17 WARP seg shows them DISABLED (tooltip "Phase 5");
  the engine plays such chops as Varispeed if a hand-edited state contains
  them.
- **UI**: chop editor overlay (800×468 at 150,120; scrim/Esc/✕ close) with
  the v17 sample editor — S/E flags (blue #5b87f5, zero-cross snap, ±7 px hit
  zones, double-click resets), fade corner grips + wedges, loop brace + bars
  + xfade diamond (60 px ≈ 200 ms, Loop only), seconds ruler, reverse
  chevron, click-to-audition from that point (reverse plays backwards, Loop
  wraps). FX page: MAIN|FX seg + Tab/M/F (both pages stay alive, VBlank keeps
  ticking); CHOP column = PLAYBACK (mode/reverse/warp mirrors — same model
  fields, separate attachments) + ENVELOPE (4-handle AHDSR display, hovered
  handle shows its value, knobs mirror); MASTER column = Phase 4 placeholder.
  Chop chip + '…' in the grid's lane row (lanes placeholder kept). Selection
  sync: strip click / box double-click / keys 1–8 / FX arrows all drive ONE
  selection (WaveformDisplay.onSelectionChanged).
- **Audition playhead is the blue accent hairline**, not v13's white: the
  "white playhead / dark screen" note in v17's chop_editor is inherited from
  the v13 dark skin; v17's own selftest says the editor draws dark strokes on
  light panels, where white would be invisible.
- **Double-click reconciliation**: Manual slice mode keeps
  double-click-adds-marker (spec §4); in the other modes double-clicking a
  chop opens the overlay (v17). '…' and Enter open it in every mode.
- **MIDI base-note test hooks unchanged**; new dev hook
  `CHOPLINE_UI_VIEW=overlay|fx` switches the view before `CHOPLINE_UI_SNAPSHOT`
  fires (Standalone only).

## Decisions made (Phase 4 — Beat Repeat + master chain)

- **Chain order (user decision 2026-10-25)**: voices (+per-chop send taps) →
  Beat Repeat → FILTER → DISTORTION → delay/reverb RETURNS → WIDTH → OUTPUT
  gain → safety limiter. WIDTH and OUTPUT last, superseding spec §7.
- **ZERO-LATENCY master chain (deliberate product call)**: this is a live
  performance instrument (BR HOLD/H/MIDI are played in real time), so nothing
  in the master chain adds lookahead/FIR latency. Distortion oversampling is
  2× **IIR polyphase** (sub-sample group delay, reported — rounds to 0 — via
  `setLatencySamples`, constant while playing); the safety limiter is a
  clip-guarded instant-attack / ~80 ms-release gain computer with **no
  lookahead** (house rule: NOT `juce::dsp::Limiter`, which hides a fixed 4:1
  compressor at −10 dBFS). Unit-tested: never exceeds the ceiling (+1e-4) and
  is BIT-IDENTICAL below it.
- **All Phase 4 parameters are APVTS** (42 total now; ids = the v17 ids). The
  UI surfaces only the v17 set (BR: HOLD/AUTO/EVERY/GRID/CHANCE/DECAY; master
  cards per the FX page); the advanced BR params (offset, gate, pitch decay,
  accel, reverse, filter sweep, mix mode, freeze, random grid, mode=Off) exist
  with the v17 `not_surfaced_in_mockup` defaults for automation and a later
  advanced drawer. The AUTO toggle drives `br_mode` Auto↔Manual; Off is
  reachable only via automation until the drawer lands.
- **Engine/processor split**: `BlockContext` carries `BeatRepeat::Params` +
  `MasterParams` structs whose **defaults are everything-bypassed** — the
  processor fills them from cached APVTS atomics every block, while engine
  tests of Phases 1–3 stay bit-identical unless they opt in.
- **Beat Repeat engine** (`engine/BeatRepeat`): 32 s stereo ring (4 bars at
  30 BPM — the spec §7 figure at 192 kHz), allocated in prepare. Timeline =
  host PPQ (same discipline as PatternScheduler); no PPQ → a free-run
  accumulator, so manual holds work in the Standalone and with stopped
  transports. Slot 0 is the capture pass (reader tracks the write head —
  bit-exact dry); repeats apply decay^k gain, 2^(−k·pdecay/12) varispeed
  read, reverse per Alternate/All (slot 0 always forward — its audio doesn't
  exist yet), accel shrinks slots up to 8× over the gate, 3 ms crossfade at
  every slot boundary and on stop. Manual triggers quantise to the next GRID
  boundary and run while held (gate ignored); Auto rolls CHANCE at every
  interval(+offset/16) boundary and runs the GATE (a boundary mid-repeat
  re-captures). Freeze pins the slice AND the slot params (no further decay)
  and pauses ring writes. Mix modes: Mix = dry + repeats k≥1, Insert = dry
  muted while repeating, Gate = repeats only. `setRandomSeed` makes chance
  deterministic for tests. Mode Off takes a fast path that still captures
  into the ring (so enabling mid-stream has history) and is bit-transparent.
- **BR vs chop MIDI collision — RESOLVED (document for user sign-off)**: chop
  notes run base+0..63 (C1+N default), which covers C3-up. BR performance
  notes therefore take a SEPARATE configurable base (`br_midi_base`: C2–C5,
  default C3=60 per spec §7) mapping base+0..7 → 1/4, 1/8, 1/16, 1/32, 1/64,
  1/8T, 1/16T, 1/32T (grid "1/2" is not performable; spec's example starts at
  1/4). When BR mode ≠ Off, notes in [base, base+8) hold a repeat at that
  grid and NEVER trigger chops; when mode = Off they fall through to the chop
  range as before. Several held perf notes → the finest grid wins; note-offs
  are per-note. Unit-tested.
- **Per-chop delay/verb sends are now routed**: tapped post-voice, pre-master
  (spec §7 "chop tails") into two engine-owned send buses. A voice with zero
  sends renders through the exact Phase 3 fast path (bit-identical); a voice
  with sends renders into a per-voice scratch and distributes dry + sends.
  Steal tails keep the faded chop's send levels. Returns mix in AFTER the
  distortion (user chain order), so tails stay clean of the drive.
- **Master modules**: FILTER = `juce::dsp::StateVariableTPTFilter` (12 dB/oct
  fixed per the v11 decision), reso 0–100 % → Q 0.707–8. DISTORTION = 6 types
  (Soft/Hard tanh/clip, Tape = tanh + 0.15 DC offset — the absorbed v14
  saturation, Fold = sin shaper, Bitcrush 1–16 bits, Downsample 1–64×
  hold at BASE rate — intentionally not oversampled); drive → up to +24 dB;
  analytic auto-gain (unity at a 0.5 reference level; crushers exempt);
  `MasterFX::shapeSample/autoGainFor` are shared with the UI transfer curve
  and the tests. DELAY = 4 s ring, tempo-synced (v17 options in beats),
  100 ms time smoothing (tempo glide), feedback hard-bounded ±4 + NaN guard.
  REVERB = `juce::Reverb` small-plate mapping (roomSize 0.25+0.72·decay),
  reset on re-activation. WIDTH = M/S with a TPT high-pass on the side
  channel for elliptical bass mono (`master_mono` ≤ 20 Hz disables it →
  width 100 % is identity). Every audible param smoothed 20 ms; section
  bypasses crossfade 15 ms and fully-off sections are skipped
  (bit-transparent).
- **Tail length**: `getTailLengthSeconds` reports INFINITY (the delay rings
  for minutes at 95 % feedback; JUCE wrappers map infinity to the host's
  infinite-tail flag, so bounces never trim an echo).
- **Post-review hardening (self code-review, Phase 4)**: ring writes pause
  during every repeat after slot 0's crossfade (holds/freezes > 32 s can no
  longer wrap over the pinned slice); oversized host blocks (> prepared max)
  run the master chain in prepared-size chunks so the safety limiter stays
  engaged (sends dropped for that pathological block only); the delay has an
  idle fast path like the other modules (return settled at 0 → skip; clean
  ring on re-activation); delay/reverb returns fold to mono when the main bus
  is mono; merged steal tails keep the higher of the two chops' send levels;
  the editor releases a momentary H hold on focus loss/destruction and no
  longer swallows Cmd/Ctrl+H.
- **UI**: `ui::BeatRepeatPanel` + `ui::MasterPanel` replace the MAIN
  placeholders (v17 geometry; HOLD fills red + 1 px red card ring while
  repeating/holding; CHANCE/DECAY knobs use the red accent via
  `Slider::rotarySliderFillColourId`, default ink set in the LnF). H key =
  momentary hold through `keyStateChanged` + an engine atomic; the REPEAT
  HOLD header badge shows on both pages; the grid playhead turns red while
  BR is engaged. `master_more` ('…') opens the MASTER–MORE CallOutBox
  (CEILING / DLY RTN / VERB RTN) per the v17 `fl_mastermore` flyout — NOT a
  jump to the FX page. FX page gained the full MASTER column: six cards,
  titles top-left + iOS mini-toggle bypass top-right inside each card,
  bypassed card dims to 45 % (tick-driven alpha), displays (filter response,
  transfer curve, width field, out/GR meter, delay taps, reverb tail) repaint
  only when their values change. BPM readout lives in MASTER on both pages
  (hairline pill; host BPM or free-run BPM), fed by an engine atomic.
- **Meters**: limiter GR + output peak ride engine atomics (same lock-free
  discipline as the MIDI flash); the MAIN lamp and the FX OUT/GR bars go
  blue while limiting (v17 faders_meters).
- **Free-run BPM stays in the settings flyout** (it is the free-run tempo
  CONTROL; the MASTER BPM readout is display-only per v17 `host_readonly`).

## Decisions made (Phase 5 — lanes, generators, slots, granular/freeze, presets, eco, drag-out)

- **Lane editor** (v17 lane_tab + step_lane replace the placeholder, exact
  geometry): one lane visible at a time (VEL 0–127 / PITCH ±24 st bipolar from
  centre, step 1 / PROB 0–100 % / RATCHET 1,2,3,4,6,8 drawn as N slivers).
  Only steps WITH events show editable bars; click/drag paints values across
  steps; double-click resets a step; hovered/dragged bar shows the mono value
  pill; a paint gesture is ONE UndoManager transaction. Bars are positioned by
  event time (off-grid events paint at their true position); the lane shows
  the current bar PAGE of the ACTIVE slot.
- **Ratchet playback**: the scheduler subdivides the event span into N equal
  retriggers riding the swung onset; each sub-trigger carries span/N as its
  gate (Gate mode gives the classic chopped ratchet; Fit stretches into the
  subdivision). **Fixed musical decay: 0.8× velocity per hit (≈ −1.9 dB)** —
  documented constant `engine::ratchetDecay`. Probability rolls ONCE per step
  occurrence (not per sub-trigger).
- **Determinism (spec §5 "repeatable" + reproducible bounces)**: new
  `engine/DetHash.h` (SplitMix64). PROBABILITY is keyed by a hash of
  (pattern seed, slot, cycle index, event position + event index — layered
  same-step events roll independently) — never a running RNG —
  so offline bounces fire exactly like the live pass at ANY block size
  (unit-tested at 256 vs 1024). **BR AUTO chance is now keyed by the interval
  boundary index** (closes the Phase 4 open question; `setRandomSeed` still
  seeds it for tests; `rnd grid` keeps the juce::Random — advanced, unsurfaced).
  Granular jitter streams are `DetRng`s seeded per trigger from
  (voice stamp, chop, stream) — stamps repeat in a fresh render, so
  bounce == bounce (unit-tested sample-exact, incl. BR auto + granular).
- **Per-step reverse override**: event field `rev` (model + snapshot), played
  as an XOR against the chop's reverse flag at the voice. Surfaced ONLY
  through Randomize — it is deliberately NOT a lane (v17 has four lanes);
  spec §5's reverse lane is folded into the generator per the brief.
- **Per-step pitch** is applied at the VOICE: effective pitch = clamp(chop ±24)
  of chop pitch + step pitch, routed through whichever warp engine is active
  (varispeed rate, stretch transpose, granular/freeze grain rate) —
  unit-tested within 2 cents on all four engines.
- **Generators** (spec §5; UI home = right-click context menu on the boxes
  tray AND the lane display — lean, no new chrome per v17): Randomize
  (seed + amount stored per pattern, serialized, themselves not undoable like
  freeBpm), Reroll seed, amount submenu 25/50/75/100 %, Shift left/right (one
  step of the current grid, wrapping), Reverse pattern (mirrors event starts),
  Copy/Paste pattern (internal clipboard) and Copy-to-slot A–H submenu.
  Randomize algorithm (deterministic per seed+amount): an amount-sized subset
  of events gets its CHOP assignments permuted (Fisher-Yates); every event
  rewrites pitch (musical set −12…+12 scaled by amount, hit rate 0.6·amount),
  reverse flip (0.3·amount) and ratchet (0.35·amount, weighted to 2/3/4).
  Each generator call = one undo transaction (unit-tested round trips).
- **Pattern slots (spec §5)**: new APVTS `pattern_slot` choice A–H (default A).
  The PatternSnapshot now carries ALL 8 slots; the scheduler owns the playing
  slot — a parameter change **quantises to the next bar start while the
  transport runs** (the block is split at the boundary sample: old slot before,
  new slot from the boundary) **and applies immediately when stopped** or on a
  pattern (re)start. MIDI PROGRAM CHANGE selects the slot (captured lock-free
  in processBlock, applied to the parameter on the message thread by a 20 Hz
  housekeeping timer, which also mirrors the param into the model's UI-edited
  slot). STILL no header slot UI — the v17 deferral stands (open question:
  bring back the v12-style single pattern button?).
- **Granular warp (spec §6)** — engine in ChopVoice, zero audio-thread
  allocation (12-grain pool per unison stream, preallocated; Hann lookup table
  filled in prepare): time runs 1:1 (read position advances one frame per
  output sample; Loop mode wraps it), pitch transposes per-grain read rate.
  **Musical fixed defaults, per-chop state fields (serialized `gs gd gj gp gy`,
  no v17 knobs — advanced-drawer candidates)**: grain 80 ms (5–200), density 3
  overlapping streams (1–8), position jitter 12 ms (0–50), pitch jitter
  6 cents (0–100), spray 0 ms (0–500). Grain amplitude compensation
  1/max(1, d/2); grains prefill staggered so the onset reaches steady state.
  Unison on granular = per-stream jitter seeds + the unison cents detune the
  grain rates (documented choice). Fit step still forces the stretch engine.
  KNOWN CHARACTER: with position jitter forced to 0, overlapping grains add a
  coherent phase-slip detune (inherent to granular clouds; the default jitter
  randomises it away) — the pitch-accuracy unit test therefore measures
  grain-level pitch with non-overlapping grains.
- **Freeze warp (spec §6)**: the granular engine with the read position PINNED
  at the playback start (chop start + startOffset, or the audition point) —
  fixed params 100 ms grains / density 4 / 4 ms jitter / no pitch jitter.
  Fills the EVENT length (Gate span; release still applies); a span-less
  One-shot freeze caps at one chop's worth so it can never ring forever.
  Declick/AHDSR apply unchanged (env multiplies the grain sum).
- **Warp seg enabled**: Granular/Freeze are selectable in the overlay + FX
  mirrors (the Phase 3 "disabled, Phase 5" state removed).
- **Preset system (spec §10; v17 header browser)**: XML presets in
  `~/Documents/CHOPLINE/Presets/<Category>/<Name>.xml`, categories Chops /
  Glitch / Stutter / Tape / Granular / User (+ any extra directory found).
  A preset IS the full state tree, so loading uses the exact glitch-safe
  session-restore path (`applyStateTree`). **Kit-only** strips the embedded
  FLAC + path/hash (keeps the sample BPM) and marks `kitOnly`; **loading a
  kit onto the current sample keeps the sample** — mapping rule (documented):
  stored marker TIMES clamp into the current sample (markers at/past its end
  drop), per-chop settings ride their markers, and pattern events pointing at
  chops beyond the count stay silent. Header combo lists presets under
  category headings + a "Save preset…" item; prev/next step through with
  wrap; the Save dialog (name / category / include-sample toggle) is a
  CallOutBox PARENTED TO THE EDITOR (v17 flyouts sit inside the plate; also
  makes component snapshots capture them — the settings flyout was moved
  in-window too). NO factory content ships this phase (user supplies samples
  and tunes the 30 factory presets later; the dev vocal is never embedded).
- **Eco/HQ (spec §8)**: new APVTS `quality` choice (Eco|HQ, default HQ),
  surfaced as a seg row in the settings flyout (v17 fl_set). Eco =
  Signalsmith **presetCheaper** stretchers + **granular density capped at 2**
  per trigger + **distortion oversampling dropped for Soft clip and Tape**
  (smooth low-order saturators — aliasing negligible at musical drives; Hard
  clip and Fold keep 2×, Bitcrush/Downsample are base-rate by design).
  Switching quality re-warms the stretcher pool, which is not audio-safe, so
  the processor's housekeeping timer wraps it in `suspendProcessing` and
  resets voices — a quality toggle briefly silences ringing voices
  (documented cost). **Measured CPU (Release, this M-series Mac, 48 kHz/256,
  8 stretch voices + ratchets + BR Auto + full master): HQ 1.84 %, Eco 1.24 %
  of one core — comfortably inside the spec §8 < 5 % target.**
- **Drag-out (spec §10)**: the drag source is the EMPTY grid-header area
  between the bar seg and the SWING knob (least-chrome option, documented;
  tooltip-free zone — the progress hairline appears under it). Plain drag =
  the current pattern as a `.mid` (960 PPQ, tempo meta; ratchets export as
  their audible subdivision with the 0.8× decay; probability events export
  unconditionally — the file is the pattern, not one roll; the per-step pitch
  lane has no MIDI representation and is not exported). **Alt-drag = audio**:
  a background thread renders the pattern through a PRIVATE offline engine
  (full chain: voices + BR + master, forced HQ per spec §8), TWO cycles with
  the second kept so tails wrap into the loop start (seamless loop); the
  subtle blue progress hairline shows in the header zone, and the drag starts
  as soon as the file is ready (release early → re-renders on the next
  Alt-drag). Temp files live in a per-editor-instance folder of the system
  temp dir, deleted on editor destruction. Export-all-chops (24-bit WAVs of
  each chop's trimmed region, raw, no FX — documented) lives in the settings
  flyout.
- **Space = transport preview (v17 keyboard map)**: the Standalone free-run
  now plays ONLY while Space is held (momentary, like H; released on focus
  loss/close). Implemented as `BlockContext.freeRunGate` defaulting TRUE, so
  hosts (their transport gates) and all earlier-phase tests are untouched;
  the processor passes the gate only for the Standalone wrapper. This closes
  the Phase 2 "free-runs permanently" known issue.
- **UI polish (spec §9, within the locked v17 look)**: WaveformDisplay now
  repaints only narrow strips around moving playheads (full repaint only when
  the playing-chop set/loading changes) — closes the Phase 1 dirty-rect note;
  the boxes display caches its cell geometry (bounds + note length keyed) —
  closes the Phase 2 note; LEFT/RIGHT arrows nudge the selected events one
  step (wrapping); tooltips across the header, grid, lanes, BR, master,
  overlay and FX knobs (`CompactKnob::setTooltip` forwards to its slider).
- **WCAG AA audit (report only, spec §9)**: ink/label/dim text on the light
  surfaces measure 8.5:1–19.4:1 (PASS AA normal). Flagged: `textTertiary`
  #a4a4aa on displays = 2.42:1 (fails AA — it is the DELIBERATE disabled/dim
  tone); accent blue on paper = 4.24:1 (passes 3:1 large-text/UI-component,
  fails 4.5:1 normal text — blue is a thin accent, not body text); BR red on
  white = 3.08:1 (passes 3:1; the REPEAT HOLD badge text is small — consider
  darkening if it ever bothers); idle MIDI dot #c7c7cc = 1.57:1 (non-text
  indicator below the 3:1 non-text bound — flagged, not redesigned).
- **44 APVTS parameters** (42 + pattern_slot + quality); schemaVersion stays 1
  (every addition is an additive child/property; Phase 1/2/3/4-shaped states
  load with defaults — unit-tested).
- **Tests**: 97 Catch2 cases / ~1.5 M assertions. New Phase 5 coverage: pitch
  lane within 2 cents through all four warp engines; ratchet counts + EXACT
  sample timing at block sizes 1–4096; probability 0/100 + seeded, block-size-
  independent determinism; BR AUTO determinism (two passes bit-identical);
  generator determinism/round-trips/undo; bar-quantised + stopped-immediate
  slot switching; granular/freeze finite + event-length fill + declick-clean +
  one-shot-freeze termination; preset scan/full/kit/mismatch mapping; MIDI
  export parse-back; offline render determinism; Eco+HQ clean; Phase 4-shaped
  state load; the allocation detector now runs granular + freeze + ratchets +
  probability + pitch lanes on top of the Phase 4 everything-on scenario.
  Hidden `[.bench]` case prints the CPU numbers above.
- **Post-review hardening (scoped self code-review, Phase 5)**: off-thread
  `setStateInformation` replaces the APVTS SYNCHRONOUSLY again (only the
  engine restore hops to the message thread — an offline bounce renders its
  first blocks with the restored parameters); Space is swallowed/handled in
  the STANDALONE only (in hosts it stays the DAW play shortcut even when the
  editor has focus); the probability hash gained event identity (layered
  same-step events no longer fire in lockstep); the Hann table fill is a
  `std::call_once` (concurrent prepares of multiple instances / the offline
  render engine are race-free); picking an unreadable preset rolls the combo
  back to the loaded one; LaneDisplay captures each EventView once per paint
  (no second locked tree scan per bar); the processor stops its housekeeping
  timer first in its destructor; duplicate BR knob tooltips removed.

## Known issues / notes

- **auval not run locally**: validating an AU requires the component to be
  installed in `~/Library/Audio/Plug-Ins/Components`, and nothing is
  installed to system folders during development. CI installs into the
  (disposable) runner and runs `auval -v aumu Chpl Manu`.
- Local pluginval run is strictness 10 with GUI tests; CI adds
  `--skip-gui-tests` (headless runners).
- **Sample + slice snapshots publish independently** — after a load there is a
  one-block window where a trigger can pair the new sample with the previous
  slice snapshot. Voice bounds-clamping makes it safe (no crash/overread);
  revisit with a shared serial if it ever becomes audible.
- **macOS TCC in the Standalone**: loading a file from a protected folder
  (Desktop/Documents) via a path the app did not get from a file chooser or
  drag can block on the system permission prompt (found via the dev autoload
  hook). Normal UI loads (chooser / drag-and-drop) carry consent and are fine.
- **During a session-rate change**, ringing voices finish on the old-rate
  snapshot (brief pitch offset); in practice `prepareToPlay` resets voices.
- **Transient defaults by ear pending**: default SENS 55 / min 80 ms gave 9
  chops on the 34 s dev vocal — musically plausible but tune with the user in
  DAW testing (Phase 1 user test).
- ~~Waveform repaints the whole display on animated frames~~ RESOLVED in
  Phase 5 (dirty-rect strips around moving playheads).
- **Pattern events spanning a bar-page boundary** render only on the page
  where they start (playback is unaffected); the Phase 5 lane bars behave the
  same way (bars draw on the page where the event starts).
- ~~Standalone free-runs the pattern permanently~~ RESOLVED in Phase 5: the
  pattern plays only while the Space transport-preview is held (or a host
  transport runs).
- **Undo via Cmd/Ctrl-Z needs keyboard focus** on the grid or the editor;
  some hosts swallow Cmd-Z before the plugin sees it (standard JUCE
  limitation). Edits remain undoable via the model regardless.
- The boxes display caches its CELL geometry since Phase 5 (bounds + note
  length keyed); event pieces still rebuild per paint (≤256 events, message
  thread only) — measured fine.
- ~~Stretch CPU is untested against the spec §8 budget~~ MEASURED in Phase 5
  (Release, M-series, 48 kHz/256, 8 stretch voices + ratchets + BR + full
  master): HQ 1.84 %, Eco 1.24 % of one core — inside the < 5 % target.
- **Gate length / Fit ratio are frozen at trigger time** — a tempo change
  mid-note doesn't re-scale a ringing gate or an in-flight Fit stretch (the
  next trigger follows the new tempo). Matches how most samplers behave.
- **Steal tails are raw PCM** (no sample/stretcher references), so they
  survive — and are ignored by — sample/snapshot swaps and the GC by design.
- **prepareToPlay warms 24 stretchers** (~100 ms worst case, message thread,
  skipped when rate/block size are unchanged). First-open cost only.
- **VoiceView normalised positions are relative to the EFFECTIVE (trimmed)
  chop**; Strip A playheads still map them over the untrimmed marker span, so
  a heavily trimmed chop's strip playhead is slightly offset (the overlay
  editor maps it correctly). Cosmetic; revisit with the Phase 5 polish.

- ~~Phase 4 — BR auto chance is NOT seeded per project~~ RESOLVED in Phase 5:
  the AUTO roll is keyed by a hash of the interval boundary index on the
  musical timeline, so bounces repeat exactly (`rnd grid` remains live-random
  — advanced, unsurfaced).

- **Phase 5 — quality toggle silences ringing voices** for ~100 ms: re-warming
  the stretcher pool is not audio-thread safe, so the switch suspends
  processing and resets voices (documented product cost; automation of
  `quality` mid-performance is therefore not recommended).
- **Phase 5 — granular coherent phase-slip**: with position jitter at 0,
  overlapping grains of a pitched cloud detune audibly (inherent artifact);
  the default 12 ms jitter randomises it. If a user zeroes jitter via a future
  advanced drawer, expect that character.
- **Phase 5 — audio drag-out needs the render to finish inside the gesture**
  (or a second Alt-drag): external file drags can only start while the mouse
  is down. 1–4 bar patterns render in well under a second, so in practice the
  first drag works; the progress hairline covers the rest.
- **Phase 5 — lane bars for spanned events** cover the whole span (one bar per
  EVENT, not per step cell) — matches "per-step overrides" since an event IS
  one placed step (spans are one event).
- **Phase 5 — kit-preset APVTS**: loading any preset (full or kit) restores
  the saved APVTS too (master chain, BR, swing, slots, quality) — intended
  ("a preset is the whole instrument"); flag if kit-only should keep the
  current master settings instead.
- **Phase 4 — BR ring writes pause while repeating** (after slot 0's
  crossfade): the pinned slice survives holds/freezes of any length; the only
  cost is that the ring's dry history restarts at the next (re)capture —
  exactly what a new capture needs anyway.
- **Phase 4 — BPM readout pill is drawn twice** (MasterPanel + FxPage, same
  v17 widget, different geometry). Extract a shared BpmReadout component in
  the Phase 5 polish pass before restyling it.
- **Phase 4 — delay time changes glide** (100 ms smoothed read head), so
  tempo automation produces a short varispeed pitch bend in the tail —
  intentional (musical) rather than a click or a re-clear.

## Open questions

- **DECIDED (user, 2026-10-25): master chain order** — WIDTH and OUTPUT go
  LAST: Beat Repeat → Filter → Distortion → Delay/Reverb returns → Width →
  Output gain → safety limiter. (Supersedes spec §7's width-before-delay
  ordering.)
- **Rename pending**: the user will change the plugin's name later
  (decision 2026-10-25, before release). Keep `PLUGIN_CODE Chpl` etc. until
  the new name lands; expect a rename pass (CMake PRODUCT_NAME, wordmark,
  bundle IDs, docs) in a later phase.

- **JUCE licence tier**: confirm the JUCE 9 revenue-cap tier and buy the
  right licence before any public release (see THIRD_PARTY_LICENSES.md).
- **DECIDED (record only)** — master-only filter with fixed 12 dB/oct slope:
  spec §6 lists a per-chop state-variable filter with 12/24 dB slopes; the
  locked v17 design supersedes it (filter is master-only, HP removed,
  12 dB/oct fixed; per-chop distortion also removed in favour of a 6-type
  master distortion absorbing saturation; tilt/tone removed). Build phases 3–4
  follow v17, not spec §6/§7, where they conflict.
- Spec asks for SPEC.md "in the repo root"; in this monorepo it lives at
  `plugins/CHOPLINE/SPEC.md` (the plugin's own root) — flag if a copy at the
  monorepo root is wanted.
- **Odd time signatures**: a pattern bar follows the host bar length, but the
  v17 box counts (4/8/16 per bar …) assume 4/4 — in 3/4 a "1/16" step is 1/16
  of a 3-quarter bar (i.e. a 1/12 note). Most step sequencers do exactly this;
  confirm with the user if true note-value steps are wanted in odd meters.
- **Swing feel**: implemented literally per spec — the even step is delayed by
  swing% of one step (75 % max keeps order). If the user expects MPC-style
  percentages (50 % = straight), remap the knob in Phase 5 polish.
- **MACROS (spec §10: 8 macros × 8 targets) are DESCOPED from Phase 5**: the
  locked v17 UI has no home for them (no macro strip, knobs or drawer). The
  parameter architecture doesn't block them (APVTS + per-chop model both
  addressable); decide in Phase 6 whether they return with an advanced
  drawer/settings page or get cut for 1.0.
- **Pattern-slot UI (user decision pending)**: slots are fully functional via
  the `pattern_slot` parameter + MIDI program change, but the header has no
  slot control (v17 "remove abcdefg" deferral upheld). Should the v12-style
  single pattern button (click cycles, hold/right-click dropdown) return?
- **Granular knobs**: grain size/density/jitter/pitch-jitter/spray are
  per-chop state with musical fixed defaults (80 ms / 3 / 12 ms / 6 c / 0) and
  serialize, but have no UI — prime advanced-drawer candidates together with
  the Phase 4 BR advanced params and formant preserve.
- **Pitch envelope / glide and scale lock (spec §6) remain deferred**: v17 has
  no UI for them (no glide/curve/scale controls anywhere). Engine-side they
  would slot into the per-chop settings + voice pitch path; decide in Phase 6
  (advanced drawer?) or cut.
- **Formant preserve has no v17 control** (spec §6 wants on/off + shift
  ±12 st): the engine stores per-chop formantPreserve (default ON, the vocal
  default) and it round-trips, but nothing surfaces it. Candidate for the
  Phase 5 advanced drawer; formant SHIFT is not implemented yet.
- **Unison spread constants** (cents/pan tables, 1/√N) are engineering
  guesses — tune by ear with the user on vocals during the Phase 3 test.
- **Choke group UI**: spec §6 default (all chops group 1) is live, but there
  is no way to change groups from the UI (v17 has none). Decide in Phase 5
  whether groups get surfaced or stay a fixed behaviour.
- **Phase 4 — BR vs chop MIDI ranges (user sign-off wanted)**: resolution
  documented under "Decisions made (Phase 4)": BR performance notes own
  [br_midi_base, +8) whenever BR mode ≠ Off and win over chop triggering
  there; with the default chop base C1 and ≥ 25 chops, chops 25–32 are
  shadowed by the C3 performance octave. If that bites, move `br_midi_base`
  (C2–C5 in settings/advanced later) or set BR mode Off. Confirm the default
  feels right in the DAW test.
- **Phase 4 — advanced BR drawer**: offset/gate/pitch-decay/accel/reverse/
  sweep/mix-mode/freeze/random-grid are engine-complete + automatable but
  have no UI (v17 `not_surfaced_in_mockup`). Decide the drawer's home in
  Phase 5 (settings flyout? FX-page card?).
- **Phase 4 — spec §7 "Gate 1/16–4 bars" units** read as note values; the
  engine treats the list as bar fractions in 4/4 (1/16 bar = a 1/16 note),
  matching the grid list. Only differs in odd meters (same caveat as the
  pattern bar note above).
