# Stage 3 (GUI) Integration Checklist - v5

**Plugin:** CORRUPTR
**Mockup Version:** v5
**Window Size:** 1200x800px fixed viewport (non-resizable); 1200x1650px total
scrollable content, internal vertical scroll (see v5-ui.yaml `window` block)
**Parameters:** 56 total (26 Float, 18 Bool, 12 Choice)
**Generated:** 2026-09-02

## Overview

This checklist guides GUI-stage integration of the CORRUPTR WebView UI
mockup. v5 is a small, surgical iteration on v4 (monochrome modular
channel-strip redesign) that adds two module enable/disable power-icon
toggles (`sequencerEnabled`, `modMatrixEnabled`) - see `v5-ui.yaml`'s header
comment for the full v4->v5 diff. This is CORRUPTR's FIRST finalized mockup
version (no prior locked `parameter-spec.md` existed) - `parameter-spec.md`
was generated fresh alongside this checklist by reconciling
`parameter-spec-draft.md` against the mockup's actual 69 bound controls.

**Key files:**
- `v5-ui.html` -> Copy to `Source/ui/public/index.html`
- `v5-PluginEditor.h` -> Reference for member declarations (56 relays + 56 attachments)
- `v5-PluginEditor.cpp` -> Reference for initialization sequence
- `v5-CMakeLists.txt` -> CMake configuration snippet
- `../parameter-spec.md` -> Locked parameter contract (56 parameters, generated alongside this checklist)

## 1. Copy UI Files

```bash
# Create UI directory structure
mkdir -p plugins/CORRUPTR/Source/ui/public/js/juce

# Copy production HTML
cp plugins/CORRUPTR/.ideas/mockups/v5-ui.html \
   plugins/CORRUPTR/Source/ui/public/index.html

# Copy JUCE frontend library from a working plugin (identical across plugins)
cp plugins/GainKnob/Source/ui/public/js/juce/index.js \
   plugins/CORRUPTR/Source/ui/public/js/juce/index.js

cp plugins/GainKnob/Source/ui/public/js/juce/check_native_interop.js \
   plugins/CORRUPTR/Source/ui/public/js/juce/check_native_interop.js
```

**Verify:**

- [ ] `Source/ui/public/index.html` exists
- [ ] `Source/ui/public/js/juce/index.js` exists
- [ ] `Source/ui/public/js/juce/check_native_interop.js` exists

## 2. Create the 56 APVTS Parameters (Stage 1/2 - Foundation)

**⚠️ CRITICAL - read `../parameter-spec.md` in full before writing
`createParameterLayout()`.** It is the locked contract for all 56
parameters (ranges, defaults, skew, choice lists). Three things need
special attention:

1. **`filterCutoff`'s skew factor is `0.3`** (from `v5-ui.yaml`), not linear -
   use `juce::NormalisableRange<float>(20.0f, 20000.0f, 0.0f, 0.3f)` (or the
   `NormalisableRange` skew-from-midpoint constructor) so the knob's turn
   feels logarithmic across the full 20Hz-20kHz range.
2. **Bypass vs. Enable polarity is NOT consistent across the 7 new v5
   toggles** - do not copy/paste defaults blindly:
   - `graphBypassSaturation`, `graphBypassWaveshaper`, `graphBypassBitcrush`,
     `graphBypassFilter`, `graphBypassGlitch`: default `false`, `true` = bypassed.
   - `sequencerEnabled`, `modMatrixEnabled`: default `true`, `true` = ON
     (**opposite polarity** - true means the module is active, not bypassed).
3. **`filterCutoff` and `glitchProbability` are each bound to TWO UI
   controls** (base knob + ADV-revealed fine-adjust mirror) but are each
   still exactly ONE APVTS parameter - do not create duplicate parameters
   for `node_filter_cutoff` / `node_glitch_probability`.

**Verify:**

- [ ] All 56 parameter IDs match `parameter-spec.md` exactly (case-sensitive)
- [ ] `filterCutoff` uses skew `0.3`, range 20.0-20000.0 Hz, default 20000.0
- [ ] The 5 `graphBypass*` toggles default to `false`
- [ ] `sequencerEnabled` and `modMatrixEnabled` default to `true`
- [ ] `mix` and `macroMix` both use range 0.0-200.0% (allow >100% overdrive)
- [ ] `inputGain`/`outputGain` use range -24.0 to +24.0 dB
- [ ] All 12 Choice parameters' `AudioParameterChoice` choice-string arrays
      match `parameter-spec.md` exactly, in order (index 0 = first choice)

## 3. Update PluginEditor Files

**Replace PluginEditor.h:**

```bash
cp plugins/CORRUPTR/.ideas/mockups/v5-PluginEditor.h \
   plugins/CORRUPTR/Source/PluginEditor.h
```

**Replace PluginEditor.cpp:**

```bash
cp plugins/CORRUPTR/.ideas/mockups/v5-PluginEditor.cpp \
   plugins/CORRUPTR/Source/PluginEditor.cpp
```

**Verify member order in PluginEditor.h:**

- [ ] Relays declared BEFORE webView
- [ ] webView declared BEFORE attachments
- [ ] Total: 56 relays (26 `WebSliderRelay`, 18 `WebToggleButtonRelay`, 12 `WebComboBoxRelay`)
- [ ] Total: 56 attachments (matching relay types 1:1)

**Verify initialization order in PluginEditor.cpp:**

- [ ] Relays created FIRST (before WebView construction)
- [ ] WebView created with `.withOptionsFrom()` for all 56 relays
- [ ] Attachments created LAST (after WebView construction)
- [ ] All attachments use the 3-parameter constructor `(parameter, relay, nullptr)`
      (Pattern #12 - JUCE 8 requires the undoManager argument)
- [ ] `setSize(1200, 800)` and `setResizable(false, false)` present
      (NOT `setSize(1200, 1650)` - 1650 is scrollable content height, not window size)

## 4. Update CMakeLists.txt

**Append WebView configuration snippet:**

```bash
# Open CMakeLists.txt and ADD v5-CMakeLists.txt content
# Location: After juce_add_plugin(), before the final target_compile_definitions()
```

**Verify:**

- [ ] `juce_add_binary_data(CORRUPTR_UIResources ...)` present
- [ ] Binary data sources include all 3 files (index.html, index.js, check_native_interop.js)
- [ ] `target_link_libraries` includes `CORRUPTR_UIResources`
- [ ] `target_link_libraries` includes `juce::juce_gui_extra`
- [ ] `JUCE_WEB_BROWSER=1` definition present
- [ ] `JUCE_USE_CURL=0` definition present
- [ ] `NEEDS_WEB_BROWSER TRUE` present in `juce_add_plugin()` (CRITICAL for VST3 - Pattern #9)

## 5. Build and Test (Debug)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

**Verify:**

- [ ] Build succeeds without warnings
- [ ] No WebView-related errors
- [ ] Binary data embedded correctly

**Test standalone:**

- [ ] Plugin window opens at 1200x800px (fixed, non-resizable)
- [ ] UI loads and displays correctly (light monochrome modular channel-strip
      look; near-black icon tiles; the ONLY color accent is the blue filter-EQ
      response curve)
- [ ] Scrolling the window vertically reveals Macros/Performance/XY Pad (2nd
      unit), then Sequencer, then Mod Matrix - full content is ~1650px tall
- [ ] All 6 main channel-strip modules visible: Distortion, Bitcrush, Glitch,
      Center Master, Filter & EQ, Feedback+Delay
- [ ] Right-click -> Inspect works (shows console)
- [ ] Console shows no JavaScript errors
- [ ] `window.__JUCE__` object exists in console

## 6. Test Parameter Binding

**Load in DAW (Ableton, Logic, Reaper) or standalone:**

- [ ] All 56 parameters sync UI <-> APVTS
- [ ] Automation updates UI (move knobs/faders/toggles via DAW automation)
- [ ] Preset recall updates UI (save preset, change values, recall)
- [ ] Values persist after reload (close DAW/standalone, reopen project)

**Test dual-binding pairs (same parameter, two controls - MUST stay in sync):**

- [ ] `filterCutoff`: dragging the base 'Cutoff' knob moves the ADV-revealed
      'Cutoff Fine' knob in real time, and vice versa
- [ ] `glitchProbability`: dragging the base 'Probability' knob moves the
      ADV-revealed 'Prob Fine' knob in real time, and vice versa

**Test bypass vs. enable polarity (do NOT assume all toggles behave the same):**

- [ ] `graphBypassSaturation`/`graphBypassWaveshaper`/`graphBypassBitcrush`/
      `graphBypassFilter`/`graphBypassGlitch`: default OFF (unlit icon),
      clicking turns the stage OFF (bypassed) - icon lights when bypass is engaged
- [ ] `sequencerEnabled`/`modMatrixEnabled`: default ON (lit icon), clicking
      turns the module OFF and dims the rest of that card to ~40% opacity
      (icon un-lights when disabled - opposite lit/unlit meaning from the
      graphBypass* toggles above)

**Test module-specific interactions:**

- [ ] Distortion module's `distortionAlgorithm` icon-tile chevrons cycle all
      12 algorithm choices, dot indicator tracks position
- [ ] Glitch module's `glitchMode` icon-tile chevrons cycle all 18 mode choices
- [ ] Filter & EQ module's `filterType` icon-tile chevrons cycle all 7 topology choices
- [ ] XY Pad: dragging the handle updates BOTH `xyPadX` and `xyPadY` smoothly
      with glide/trail (not instant jumps)
- [ ] Sequencer: `sequencerRate`/`sequencerSteps` combos change; Random/Mutate/
      Reverse/Mirror/Shift/Clear action buttons trigger (UI-only actions, not
      APVTS parameters - console-log only until Stage 2/3 wires real pattern logic)
- [ ] Mod Matrix: all 4 LFO mini-panels' Rate knob, Shape combo, and Sync
      toggle each bind their own independent parameter (`lfo1Rate`..`lfo4Sync`)
- [ ] ADV pills (Distortion, Glitch, Filter & EQ, Macros) show/hide their
      revealed controls - these are UI-only interaction state, NOT parameters

## 7. WebView-Specific Validation

**CSS constraints:**

- [ ] No viewport units in CSS (`100vh`, `100vw`, `100dvh`) - `v5-ui.html`
      uses `html, body { height: 100%; }` and `.plugin-frame { width: 100%;
      height: 100%; }`, with internal `overflow-y: auto` for the ~1650px
      of scrollable content
- [ ] `user-select: none` present on body (native feel); `input[type="text"]`
      (preset search field) explicitly re-enables text selection/cursor
- [ ] Context menu disabled in JavaScript (`contextmenu` preventDefault)

**Resource provider:**

- [ ] All files return correct MIME types
  - [ ] `index.html` -> `text/html`
  - [ ] `index.js` -> `application/javascript` (NOT `text/javascript`)
  - [ ] `check_native_interop.js` -> `application/javascript`
- [ ] No 404s in console (check Network tab / dev tools)
- [ ] Explicit URL mapping used in `getResource()` (Pattern #8)

**Interaction:**

- [ ] Knobs/faders use drag interaction without lag or freezing
- [ ] No lag or freezing while scrolling the 1650px-tall content within
      the 800px viewport

## 8. Build and Test (Release)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

**Verify:**

- [ ] Release build succeeds without warnings
- [ ] **Crash test:** Close and reopen plugin 10 times in a row
  - If crashes on close: member order is wrong in PluginEditor.h
  - Verify: Relays -> WebView -> Attachments
- [ ] All 56 parameters still work correctly in Release

## Parameter List (from parameter-spec.md)

Full descriptions, ranges, defaults, and DSP usage: see `../parameter-spec.md`. Summary (56 total):

**Top Bar / Global:**
- `qualityMode` - Choice (5 options), default "NORMAL" -> `WebComboBoxRelay`

**Distortion:**
- `graphBypassSaturation` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`
- `distortionAlgorithm` - Choice (12 options), default "Saturation" -> `WebComboBoxRelay`
- `drive` - Float, 0.0 to 40.0 dB, default 6.0 -> `WebSliderRelay`
- `fold` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `graphBypassWaveshaper` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`

**Bitcrush:**
- `graphBypassBitcrush` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`
- `bitDepth` - Float, 1.0 to 16.0 bit, default 16.0 -> `WebSliderRelay`

**Glitch:**
- `graphBypassGlitch` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`
- `glitchMode` - Choice (18 options), default "Off" -> `WebComboBoxRelay`
- `glitchBufferLength` - Choice (9 options), default "1/16" -> `WebComboBoxRelay`
- `glitchProbability` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`

**Center Master:**
- `mix` - Float, 0.0 to 200.0 %, default 100.0 -> `WebSliderRelay`
- `outputLimiterStyle` - Choice (2 options), default "Transparent" -> `WebComboBoxRelay`
- `inputGain` - Float, -24.0 to 24.0 dB, default 0.0 -> `WebSliderRelay`
- `outputGain` - Float, -24.0 to 24.0 dB, default 0.0 -> `WebSliderRelay`

**Filter & EQ:**
- `graphBypassFilter` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`
- `filterType` - Choice (7 options), default "LP" -> `WebComboBoxRelay`
- `filterCutoff` - Float, 20.0 to 20000.0 Hz, default 20000.0 -> `WebSliderRelay`
- `filterResonance` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`

**Feedback + Delay:**
- `feedbackAmount` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `feedbackDamping` - Float, 0.0 to 100.0 %, default 50.0 -> `WebSliderRelay`
- `microDelayTime` *(NEW)* - Float, 0.1 to 50.0 ms, default 10.0 -> `WebSliderRelay`

**Macros:**
- `macroDamage` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `macroCrush` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `macroGlitch` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `macroChaos` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `macroRhythm` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `macroMovement` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `macroWidth` - Float, 0.0 to 100.0 %, default 50.0 -> `WebSliderRelay`
- `macroMix` - Float, 0.0 to 200.0 %, default 100.0 -> `WebSliderRelay`

**Performance Triggers:**
- `performanceKill` - Bool, default false -> `WebToggleButtonRelay`
- `performanceGlitch` - Bool, default false -> `WebToggleButtonRelay`
- `performanceDestroy` - Bool, default false -> `WebToggleButtonRelay`
- `performanceFreeze` - Bool, default false -> `WebToggleButtonRelay`
- `performanceReverse` - Bool, default false -> `WebToggleButtonRelay`
- `performanceStutter` - Bool, default false -> `WebToggleButtonRelay`
- `performanceChaos` - Bool, default false -> `WebToggleButtonRelay`

**XY Performance Pad:**
- `xyPadX` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`
- `xyPadY` - Float, 0.0 to 100.0 %, default 0.0 -> `WebSliderRelay`

**Rhythmic Sequencer:**
- `sequencerEnabled` *(NEW)* - Bool, default true -> `WebToggleButtonRelay`
- `sequencerRate` - Choice (7 options), default "1/16" -> `WebComboBoxRelay`
- `sequencerSteps` - Choice (2 options), default "16" -> `WebComboBoxRelay`

**Modulation Matrix (4 LFOs):**
- `modMatrixEnabled` *(NEW)* - Bool, default true -> `WebToggleButtonRelay`
- `lfo1Rate` - Float, 0.01 to 20.0 Hz, default 1.0 -> `WebSliderRelay`
- `lfo1Shape` *(NEW)* - Choice (9 options), default "Sine" -> `WebComboBoxRelay`
- `lfo1Sync` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`
- `lfo2Rate` - Float, 0.01 to 20.0 Hz, default 1.0 -> `WebSliderRelay`
- `lfo2Shape` *(NEW)* - Choice (9 options), default "Sine" -> `WebComboBoxRelay`
- `lfo2Sync` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`
- `lfo3Rate` - Float, 0.01 to 20.0 Hz, default 1.0 -> `WebSliderRelay`
- `lfo3Shape` *(NEW)* - Choice (9 options), default "Sine" -> `WebComboBoxRelay`
- `lfo3Sync` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`
- `lfo4Rate` - Float, 0.01 to 20.0 Hz, default 1.0 -> `WebSliderRelay`
- `lfo4Shape` *(NEW)* - Choice (9 options), default "Sine" -> `WebComboBoxRelay`
- `lfo4Sync` *(NEW)* - Bool, default false -> `WebToggleButtonRelay`

## Troubleshooting

### WebView doesn't load (blank screen)

**Check:** Resource provider returns all 3 files; MIME types correct
(`application/javascript` NOT `text/javascript`); binary data regenerated
(rebuild CMake).

**Solution:**
```bash
rm -rf build && cmake -B build && cmake --build build
```

### Controls frozen (don't respond to interaction)

**Check:** ES6 module loading (`<script type="module">` in HTML); `import
* as Juce from "./js/juce/index.js"` at top of script; all attachments use
the 3-parameter constructor (Pattern #12); `check_native_interop.js`
included in binary data and served by the resource provider.

### One specific parameter doesn't respond, others work fine

**Check:** That parameter's relay name (PluginEditor.cpp), APVTS parameter
ID (PluginProcessor.cpp `createParameterLayout()`), and HTML `data-param`
attribute (index.html) are all byte-for-byte identical, case-sensitive.
Cross-check against `parameter-spec.md`.

### sequencerEnabled/modMatrixEnabled behave backwards (dim when ON)

**Check:** These 2 toggles are the OPPOSITE polarity from the 5
`graphBypass*` toggles - default `true` (ON/lit), not `false`. Verify the
APVTS default value in `createParameterLayout()` is `true` for both.

### Release build crashes on reload

**Check:** Member order (relays -> webView -> attachments); all members use
`std::unique_ptr` (Pattern #11); constructor initialization matches
declaration order.

### VST3 doesn't appear in DAW

**Check:** `NEEDS_WEB_BROWSER TRUE` present in `juce_add_plugin()` (Pattern #9).

## Success Criteria

GUI integration is complete when:

- ✅ Plugin builds without errors (debug + release)
- ✅ UI loads in standalone app at 1200x800px, scrolls to reveal full ~1650px content
- ✅ All 12 modules/strips visible and functional (Distortion, Bitcrush, Glitch,
     Center Master, Filter & EQ, Feedback+Delay, Macros, Performance, XY Pad,
     Sequencer, Mod Matrix, Top Bar)
- ✅ All 56 parameters update correctly (C++ <-> JavaScript)
- ✅ Both dual-binding pairs (filterCutoff, glitchProbability) stay in sync
     across their base + ADV-fine controls
- ✅ Bypass toggles (5) and enable toggles (2) both work with their correct,
     opposite default polarities
- ✅ Plugin works in DAW (VST3 + AU)
- ✅ Automation and preset recall work
- ✅ No release build crashes (tested 10 reloads)

## Next Steps

After successful integration:

1. Resolve architecture.md's remaining open Stage-2 research items:
   `qualityMode=AUTO` heuristic, `macroWidth`'s pre-Stereo-module behavior,
   `distortionAlgorithm=Bitcrusher/SampleRateReducer`'s relationship to the
   always-on Bitcrush stage, and the 4 LFOs' tempo-sync division mapping
   (`lfoNSync=true` behavior)
2. Address the deferred items noted in `parameter-spec.md`'s Reconciliation
   Notes section (6 draft-only parameters with no bound UI control; 32
   Mod-Matrix-slot parameters not yet individually bound) via a future
   mockup iteration or direct product decision before those features are
   implemented
3. Continue to DSP implementation per architecture.md's 9 DSP phases
   (Phase 3.1: Feedback Routing Path built/validated in isolation FIRST;
   Phase 3.2: unified modulation accumulator built/tested in isolation
   before wiring into real DSP)
4. Run pluginval tests
5. Manual DAW testing
6. Install for production use (`/install-plugin CORRUPTR`)
