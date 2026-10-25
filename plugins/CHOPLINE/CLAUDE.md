# CHOPLINE — CLAUDE.md

Tempo-synced vocal chop / warp / glitch instrument. The authoritative contract
is `SPEC.md` (user-authored); the locked visual direction is the v17 mockup
(`.ideas/mockups/v17-ui.yaml`, see `.ideas/creative-brief.md` "UI Design — FINAL").
Work phase by phase (spec §11) and STOP after each phase for user approval.

**Current phase: 3 (Chop FX / warp engine) — complete, awaiting approval.**

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
- Waveform repaints the whole display on animated frames (playheads); fine at
  60 fps on M-series — sub-region dirty rects are a Phase 5 polish item.
- **Pattern events spanning a bar-page boundary** render only on the page
  where they start (playback is unaffected). Revisit with the Phase 5 lane UI.
- **Standalone free-runs the pattern permanently** (spec §8 "free run at
  manual BPM") — as soon as events exist they play. If that proves annoying in
  practice, gate it on a transport/preview control (Space is reserved for
  "transport_preview" in v17's keyboard map).
- **Undo via Cmd/Ctrl-Z needs keyboard focus** on the grid or the editor;
  some hosts swallow Cmd-Z before the plugin sees it (standard JUCE
  limitation). Edits remain undoable via the model regardless.
- The boxes display rebuilds its cell/piece geometry per paint (≤24 cells,
  ≤256 events, message thread only) — simple and fast enough; cache if the
  Phase 5 lane editor makes paints heavier.
- **Stretch CPU is untested against the spec §8 budget** (<5% of an M1 core
  with 8 stretch voices): per-trigger outputSeek bursts and per-stream FFTs
  are the hot spots. Measure during the Phase 3 DAW test; mitigations ready:
  presetCheaper for an Eco mode (Phase 5 quality switch), smaller shared
  unison pool.
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

## Open questions

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
- **Pitch envelope / glide and scale lock (spec §6) are deferred**: v17 has
  no UI for them (no glide/curve/scale controls anywhere). Engine-side they
  would slot into the per-chop settings + voice pitch path; ask the user
  whether they return in Phase 5 (advanced drawer?) or get cut.
- **Formant preserve has no v17 control** (spec §6 wants on/off + shift
  ±12 st): the engine stores per-chop formantPreserve (default ON, the vocal
  default) and it round-trips, but nothing surfaces it. Candidate for the
  Phase 5 advanced drawer; formant SHIFT is not implemented yet.
- **Unison spread constants** (cents/pan tables, 1/√N) are engineering
  guesses — tune by ear with the user on vocals during the Phase 3 test.
- **Choke group UI**: spec §6 default (all chops group 1) is live, but there
  is no way to change groups from the UI (v17 has none). Decide in Phase 5
  whether groups get surfaced or stay a fixed behaviour.
