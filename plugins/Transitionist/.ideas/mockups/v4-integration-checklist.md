# Stage 3 (GUI) Integration Checklist - v4

**Plugin:** Transitionist
**Mockup Version:** v4
**Window Size:** 980x420px (fixed, non-resizable)
**Parameters:** 3 total (all sliders: throw, space, sweep)
**Generated:** 2026-08-26

## Overview

This checklist guides GUI-stage integration of the Transitionist WebView UI
mockup. v4 is a **parameter-set change** from the prior 2-knob concept
(mix/duration) - it introduces three new parameters (`throw`, `space`,
`sweep`) with the light skeuomorphic hardware visual style carried forward
unchanged from v3. `sweep` is the first BIPOLAR parameter in this plugin's
history and needs special attention during Stage 1/2 (Foundation) APVTS
declaration - see the dedicated section below.

**Key files:**
- `v4-ui.html` → Copy to `Source/ui/public/index.html`
- `v4-PluginEditor.h` → Reference for member declarations
- `v4-PluginEditor.cpp` → Reference for initialization sequence
- `v4-CMakeLists.txt` → CMake configuration snippet
- `../parameter-spec.md` → Locked parameter contract (generated alongside this checklist)

## 1. Copy UI Files

```bash
# Create UI directory structure
mkdir -p plugins/Transitionist/Source/ui/public/js/juce

# Copy production HTML
cp plugins/Transitionist/.ideas/mockups/v4-ui.html \
   plugins/Transitionist/Source/ui/public/index.html

# Copy JUCE frontend library from a working plugin (identical across plugins)
cp plugins/GainKnob/Source/ui/public/js/juce/index.js \
   plugins/Transitionist/Source/ui/public/js/juce/index.js

cp plugins/GainKnob/Source/ui/public/js/juce/check_native_interop.js \
   plugins/Transitionist/Source/ui/public/js/juce/check_native_interop.js
```

**Verify:**

- [ ] `Source/ui/public/index.html` exists
- [ ] `Source/ui/public/js/juce/index.js` exists
- [ ] `Source/ui/public/js/juce/check_native_interop.js` exists

## 2. Create the "sweep" BIPOLAR Parameter (Stage 1/2 - Foundation)

**⚠️ CRITICAL - read before writing APVTS parameter layout.**

`throw` and `space` are standard unipolar float parameters (0.0-100.0,
default 0.0). `sweep` is BIPOLAR: -100.0 to +100.0 with a center detent
default of 0.0 (= fully open / no filtering). The relay/attachment/WebView
layer requires NO special handling for bipolar parameters - it always
operates on the parameter's *normalised* (0-1) value. The bipolar behavior
lives entirely in:

1. The APVTS `NormalisableRange` min/max passed at parameter creation, and
2. The HTML/JS bipolar arc-fill + signed LCD renderer (already implemented
   in `v4-ui.html`'s `makeBipolarRenderer()`).

**APVTS declaration (PluginProcessor.cpp `createParameterLayout()`):**

```cpp
// THROW - unipolar
params.push_back(std::make_unique<juce::AudioParameterFloat>(
    juce::ParameterID("throw", 1),
    "Throw",
    juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f), // linear
    0.0f   // default
));

// SPACE - unipolar
params.push_back(std::make_unique<juce::AudioParameterFloat>(
    juce::ParameterID("space", 1),
    "Space",
    juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f, 1.0f), // linear
    0.0f   // default
));

// SWEEP - BIPOLAR: min is negative, default sits at the range midpoint
// so normalised 0.5 == 0% (center detent, fully open filter)
params.push_back(std::make_unique<juce::AudioParameterFloat>(
    juce::ParameterID("sweep", 1),
    "Sweep",
    juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f, 1.0f), // linear
    0.0f   // default = center (0% = fully open)
));
```

**Verify:**

- [ ] `sweep`'s `NormalisableRange` min is `-100.0f`, NOT `0.0f`
- [ ] `sweep`'s default value is `0.0f` (center), and `0.0f` normalises to
      exactly `0.5` given the `-100..100` range (confirm with
      `sweepParam->convertTo0to1(0.0f) == 0.5f`)
- [ ] `throw` and `space` remain standard `0.0-100.0` unipolar ranges,
      default `0.0f`
- [ ] DSP code reads `sweep`'s raw (denormalised) value directly - negative
      = lowpass closing, positive = highpass opening, per creative-brief.md

## 3. Update PluginEditor Files

**Replace PluginEditor.h:**

```bash
cp plugins/Transitionist/.ideas/mockups/v4-PluginEditor.h \
   plugins/Transitionist/Source/PluginEditor.h
```

**Replace PluginEditor.cpp:**

```bash
cp plugins/Transitionist/.ideas/mockups/v4-PluginEditor.cpp \
   plugins/Transitionist/Source/PluginEditor.cpp
```

**Verify member order in PluginEditor.h:**

- [ ] Relays declared BEFORE webView
- [ ] webView declared BEFORE attachments
- [ ] Total: 3 relays (throw, space, sweep - all `WebSliderRelay`)
- [ ] Total: 3 attachments (matching relays, all `WebSliderParameterAttachment`)

**Verify initialization order in PluginEditor.cpp:**

- [ ] Relays created FIRST (before WebView construction)
- [ ] WebView created with `.withOptionsFrom()` for all 3 relays
- [ ] Attachments created LAST (after WebView construction)
- [ ] All attachments use the 3-parameter constructor `(parameter, relay, nullptr)`
      (Pattern #12 - JUCE 8 requires the undoManager argument)
- [ ] `setSize(980, 420)` and `setResizable(false, false)` present

## 4. Update CMakeLists.txt

**Append WebView configuration snippet:**

```bash
# Open CMakeLists.txt and ADD v4-CMakeLists.txt content
# Location: After juce_add_plugin(), before the final target_compile_definitions()
```

**Verify:**

- [ ] `juce_add_binary_data(Transitionist_UIResources ...)` present
- [ ] Binary data sources include all 3 files (index.html, index.js, check_native_interop.js)
- [ ] `target_link_libraries` includes `Transitionist_UIResources`
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

- [ ] Plugin window opens (980x420px)
- [ ] UI loads and displays correctly (light skeuomorphic hardware look,
      gray textured outer frame, three pale silver panel cards)
- [ ] All three knobs visible and labeled: THROW, SPACE, SWEEP (left to right)
- [ ] SWEEP shows a fixed tick mark at 12 o'clock (center detent) that
      THROW/SPACE do not have
- [ ] Right-click → Inspect works (shows console)
- [ ] Console shows no JavaScript errors
- [ ] `window.__JUCE__` object exists in console

## 6. Test Parameter Binding

**Load in DAW (Ableton, Logic, Reaper) or standalone:**

- [ ] All 3 parameters sync UI ↔ APVTS
- [ ] Automation updates UI (move knobs via DAW automation)
- [ ] Preset recall updates UI (save preset, change values, recall)
- [ ] Values persist after reload (close DAW/standalone, reopen project)

**Test each control:**

- [ ] `throw` knob: 0-100%, default 0%, arc fills clockwise from 12 o'clock start
- [ ] `space` knob: 0-100%, default 0%, arc fills clockwise from 12 o'clock start
- [ ] `sweep` knob: **-100% to +100%**, default 0% (center)
  - [ ] Turning CCW (negative) shows `-NN%` in LCD, arc fills leftward from 12 o'clock
  - [ ] Turning CW (positive) shows `+NN%` in LCD, arc fills rightward from 12 o'clock
  - [ ] Exactly at center shows `0%` (no sign) and arc has zero length
  - [ ] Double-clicking sweep resets it to 0% (center), not to the range minimum

## 7. WebView-Specific Validation

**CSS constraints:**

- [ ] No viewport units in CSS (`100vh`, `100vw`, `100dvh`) - `v4-ui.html`
      uses fixed `980px`/`420px` dimensions, not viewport units
- [ ] `user-select: none` present on body (native feel)
- [ ] Context menu disabled in JavaScript (`contextmenu` preventDefault)

**Resource provider:**

- [ ] All files return correct MIME types
  - [ ] `index.html` → `text/html`
  - [ ] `index.js` → `application/javascript` (NOT `text/javascript`)
  - [ ] `check_native_interop.js` → `application/javascript`
- [ ] No 404s in console (check Network tab / dev tools)
- [ ] Explicit URL mapping used in `getResource()` (Pattern #8)

**Interaction:**

- [ ] Knobs use relative frame-delta drag (Pattern #16), not absolute
      cursor-position tracking
- [ ] Mouse wheel nudges value by 1% per tick on all three knobs
- [ ] Keyboard arrow keys nudge value by 1% when a knob has focus
- [ ] No lag or freezing on interaction

## 8. Build and Test (Release)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

**Verify:**

- [ ] Release build succeeds without warnings
- [ ] **Crash test:** Close and reopen plugin 10 times in a row
  - If crashes on close: member order is wrong in PluginEditor.h
  - Verify: Relays → WebView → Attachments
- [ ] All 3 parameters still work correctly in Release

## Parameter List (from parameter-spec.md)

1. **throw** - Float, 0.0 to 100.0 %, default 0.0 → `WebSliderRelay` / `WebSliderParameterAttachment`
2. **space** - Float, 0.0 to 100.0 %, default 0.0 → `WebSliderRelay` / `WebSliderParameterAttachment`
3. **sweep** - Float, **-100.0 to +100.0 %** (BIPOLAR, center detent default 0.0) → `WebSliderRelay` / `WebSliderParameterAttachment`

## Troubleshooting

### WebView doesn't load (blank screen)

**Check:** Resource provider returns all 3 files; MIME types correct
(`application/javascript` NOT `text/javascript`); binary data regenerated
(rebuild CMake).

**Solution:**
```bash
rm -rf build && cmake -B build && cmake --build build
```

### Knobs frozen (don't respond to drag)

**Check:** ES6 module loading (`<script type="module">` in HTML); `import
{ getSliderState }` at top of script; all attachments use the 3-parameter
constructor (Pattern #12); `check_native_interop.js` included in binary
data and served by the resource provider.

### Sweep knob shows wrong sign or never goes negative

**Check:** `sweep`'s `NormalisableRange` in `createParameterLayout()` uses
`-100.0f` as the min (not `0.0f`); default value is `0.0f`, not `-100.0f`
or `50.0f`. A normalised value of `0.5` MUST correspond to `0%` display.

**Solution:** Re-check the APVTS declaration in Section 2 of this checklist.

### Release build crashes on reload

**Check:** Member order (relays → webView → attachments); all members use
`std::unique_ptr` (Pattern #11); constructor initialization matches
declaration order.

### VST3 doesn't appear in DAW

**Check:** `NEEDS_WEB_BROWSER TRUE` present in `juce_add_plugin()` (Pattern #9).

## Success Criteria

GUI integration is complete when:

- ✅ Plugin builds without errors (debug + release)
- ✅ UI loads in standalone app at 980x420px
- ✅ All 3 controls (THROW, SPACE, SWEEP) visible and functional
- ✅ SWEEP's bipolar behavior (signed readout, center-outward arc fill,
     center-detent tick) works correctly across the full -100%..+100% range
- ✅ Parameters update correctly (C++ ↔ JavaScript)
- ✅ Plugin works in DAW (VST3 + AU)
- ✅ Automation and preset recall work
- ✅ No release build crashes (tested 10 reloads)

## Next Steps

After successful integration:

1. Stage 0 DSP research items (per creative-brief.md): THROW's
   freeze-engagement threshold/mapping curve, SPACE's shared
   delay-division/reverb-size mapping, SWEEP's LadderFilter
   lowpass/highpass mode-switching + resonance-rise curve
2. Continue to DSP implementation (Delay → Reverb → Bipolar DJ Filter →
   Output Glue → Width signal chain, per creative-brief.md)
3. Run pluginval tests
4. Manual DAW testing
5. Install for production use (`/install-plugin Transitionist`)
