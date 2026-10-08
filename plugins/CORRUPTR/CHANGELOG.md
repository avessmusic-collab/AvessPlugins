# CORRUPTR Changelog

## v1.0.2 — 2026-10-08 (full-effect QA audit)

### Fixed
- Output limiter secretly compressed everything above -10 dBFS: `juce::dsp::Limiter`
  contains a FIXED always-on first-stage compressor (threshold -10 dBFS, ratio 4:1,
  2 ms attack / 200 ms release) plus ~+3.75 dB makeup gain, so any signal hotter than
  -10 dBFS was squashed 4:1 with program-dependent pumping even in "Transparent" mode —
  violating the v7 design intent ("signal below the ceiling passes at unity"). Replaced
  with the Limiter's own second stage alone (`juce::dsp::Compressor`, ratio 1000:1,
  1 µs attack, threshold = ceiling): genuine unity below the ceiling, brickwall at it,
  no hidden makeup; the old +ceiling post-gain compensation stage is gone too (it only
  existed to cancel Limiter's internal makeup). Expect slightly more open dynamics and
  ~1 dB less loudness on hot material; limiterCeiling/limiterRelease/Auto behave the same.
- Ring-Mod carrier phase reset every block: the per-block
  `ringModOsc.prepare()` call (added for oversampling-rate correctness) also zeroes the
  oscillator phase (`Oscillator::prepare()` calls `reset()`), producing a block-rate
  carrier discontinuity — buzz whose character depended on the host buffer size —
  whenever drive > 0. The re-prepare now happens only on an actual oversampling-factor
  change. Verified: renders at 480-sample vs 4800-sample blocks now match (mean abs
  diff 0.26 -> 0.0008).

### Added
- Console QA runner (`CORRUPTR_Tests`, mirrors KICKR's pattern): 62 offline-render
  checks covering all 12 distortion algorithms (incl. drive-0 neutrality), 6 bitcrush
  modes + SRR hold, 5 filter types + morph + slope, all 17 glitch modes, feedback-loop
  safety/decay, 4 quality modes + latency reporting, performance triggers, sequencer
  gate + mod matrix LFO modulation, and state round-trip.
  Build: `cmake --build build --target CORRUPTR_Tests`. All 62 pass; pluginval
  strictness 10 passes.

### Notes (audited, by design / deferred)
- Feedback at 100% with high drive self-oscillates indefinitely (drive re-amplifies the
  recirculated signal each pass); it stays bounded by the RMS limiter and decays
  normally at low drive. Documented as designed behavior.
- `updateFilterParameters()` would produce NaN coefficients if ever called with
  sample rate 0 — unreachable in hosts (wrapper always sets SR before processing),
  found only via a bare-metal test harness.
- Sequencer snapshot is published with a release store but read with a relaxed load;
  worst case is a one-block-stale pattern value on ARM. Cosmetic.

## v1.0.1 — 2026-09-08

### Fixed
- Six factory presets loaded the wrong filter type since the v12 Auto Filter rework
  (commit 218bc48) cut the filter type list from 8 (`LP|HP|BP|Notch|Comb|Resonant LP|
  Resonant HP|Formant`) to 5 (`Lowpass|Highpass|Bandpass|Notch|Morph`). The presets
  still carried old indices 5 (Resonant LP) and 7 (Formant); `setChoiceIndex` clamps
  out-of-range values to the last choice (Morph), and with `filterMorph` at its default
  0 the result is a plain lowpass — "Talk Box" had no vocal character at all.
  **Root cause:** preset table in `Source/ui/public/index.html` was not updated when
  the choice list shrank; index clamping made the mismatch silent.
  **Remap:** Resonant LP presets (The Screamer, Acid Bath, Auto Wah, Gate Chopper)
  → Lowpass (their high resonance values carry the character on the SVF engine);
  Formant presets (Talk Box, Vowel Movement) → Bandpass with existing high resonance.
- Stale `filterSlope` tooltip still referenced removed Comb/Formant types.
- Ring-Mod was fully audible with Drive at 0: every other algorithm's curve is
  ~identity at 0 dB drive, but `y = x * carrier` modulates at full depth no matter
  the gain. Drive now doubles as modulation depth for Ring-Mod — dry at 0 dB,
  ramping to full ring mod at 20 dB (upper half of the fader adds plain input
  gain on top). "Ring Modulator" factory preset drive raised 8 → 18 dB to keep
  its full-depth character under the new mapping.

- Resizing the window hid the Main/Sequencer/Performance tabs and the preset
  strip: the sticky top bar sat inside a zoomed `.container` while its scroll
  container was outside the zoom, and WebKit resolves sticky offsets in the outer
  coordinate space — at zoom < 1 the bar overlapped the content below it. The
  fit-to-width zoom is now applied to `body`, keeping the sticky bar and scroll
  container in one coordinate space. Verified in the standalone at 800×592.
- Resize did nothing in some hosts: the page's only rescale trigger was the
  WebView's own `resize` event, which not every host delivers when the plugin
  window changes size. `PluginEditor::resized()` now pushes the editor width
  into the page (`window.__corruptrZoomToFit`) via `evaluateJavascript`, with
  the window listener kept as fallback.
- Bottom corners of the unit cards were clipped/irregular: (a) the Main page
  measured 803px tall against the 800px design viewport, so the card's rounded
  bottom corners always sat past the window edge; (b) the 10px scrollbar gutter
  ate design width, pushing the right corner underneath it. Height re-budgeted
  (tabs pad 8→4, strip-wrap top pad 30→16, main bottom pad 15) so the Main page
  is exactly 800px — card bottom at 785 + 15px margin matching the 15px side
  margins — and the scrollbar is hidden (page-tab layout; wheel scroll still
  works on overflow). Measured in an offscreen WKWebView at 1200×800; corners
  verified whole in the standalone.

- Shrinking the window hid the bottom of the UI (volume strips): the zoom fit
  width only, assuming height = width/1.5, but runtime chrome (standalone
  banner) and runtime-only content growth (live page measured 819px vs the
  nominal 800) left a vertical overflow that scrolled. The zoom now fits
  INSIDE the window on both axes and then self-corrects against the page's
  actual rendered height (iterated — zoomed-px metrics are integers), so every
  page always fits fully, centered with page-bg letterboxing when height is
  the limiting axis. Page switches re-run the fit (each page has its own
  height; no resize event fires on a tab click). All three pages verified
  scroll-free in the standalone at 900×660.

### Changed
- Master module: Auto/Balance toggle row nudged 16px right for breathing room.

### Testing
- pluginval 1.0.4 strictness 10 on installed VST3: SUCCESS (pre-fix audit baseline;
  data-only UI change, no DSP touched).
- Full preset enum audit: all other combo values verified in range against current
  choice lists (distortionAlgorithm, bitcrushMode, glitchMode, glitchBufferLength,
  lfoShape, modSlotSource/Destination, sequencer lanes).

## v1.0.0 — 2026-09-07

- Initial installed release: 12-algorithm distortion, 18-mode glitch engine,
  host-synced 10-lane sequencer, 8-slot mod matrix + 4 LFOs, macro system, XY pad,
  performance triggers, Auto Filter-style SVF filter (v12), dual-mode limiter,
  oversampling/quality engine. Mac VST3/AU + Windows installer.
