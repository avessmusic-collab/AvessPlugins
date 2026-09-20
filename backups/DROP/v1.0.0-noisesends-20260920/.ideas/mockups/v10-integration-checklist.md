# Stage 3 (GUI) Integration Checklist - v10

**Plugin:** DROP
**Mockup Version:** v10 "Light Neumorphic / White Marble" (FINALIZED 2026-09-14, selected over v9 "Dark Iridescent")
**Generated:** 2026-09-14 by ui-finalization-agent

This checklist maps onto plan.md's existing Stage 3 GUI phase breakdown
(4.1 Layout and Basic Controls, 4.2 Parameter Binding and Interaction, 4.3
Visualization/DropGraph) rather than inventing a new phase structure - use
plan.md's own test criteria for each phase alongside the steps below.

## 0. Preconditions

- [ ] Stage 1 (Foundation) and Stage 2 (Shell, full 19-parameter APVTS) complete
- [ ] `plugins/DROP/.ideas/parameter-spec.md` exists and is locked (it does - see this same agent run)
- [ ] `plugins/DROP/.ideas/architecture.md` reviewed for the ring-buffer contract and non-parameter ValueTree state shape

## 1. Remove Superseded Mockup Versions

- [ ] `plugins/DROP/.ideas/mockups/v1-ui-test.html` through `v9-ui-test.html`/`v9-ui.yaml` and their reference images are historical design-iteration artifacts only - do NOT copy any of them into `Source/`. Only v10's files are production inputs. Leave them on disk (they are the design history / audit trail), just don't reference them from code.
- [ ] Confirm v9 was never modified during v10's iteration (v10-ui.yaml's own notes record mtime/md5 checks after every pass) - no action needed, just don't second-guess v9's presence.

## 2. Copy UI Files to Source Tree

```bash
mkdir -p plugins/DROP/Source/ui/public/js/juce

cp plugins/DROP/.ideas/mockups/v10-ui.html \
   plugins/DROP/Source/ui/public/index.html

cp plugins/DROP/.ideas/mockups/v10-marble2-texture-hd.jpg \
   plugins/DROP/Source/ui/public/v10-marble2-texture-hd.jpg

# JUCE frontend library - copy from any existing WebView plugin in this
# repo (e.g. plugins/CORRUPTR/Source/ui/public/js/juce/), do NOT hand-write:
cp plugins/CORRUPTR/Source/ui/public/js/juce/index.js \
   plugins/DROP/Source/ui/public/js/juce/index.js
cp plugins/CORRUPTR/Source/ui/public/js/juce/check_native_interop.js \
   plugins/DROP/Source/ui/public/js/juce/check_native_interop.js
```

**Verify:**
- [ ] `Source/ui/public/index.html` exists and contains v10's production markup (no `{{PLACEHOLDERS}}`, no test-harness chrome - already true of v10-ui.html as generated)
- [ ] `Source/ui/public/v10-marble2-texture-hd.jpg` exists (bundled resource asset, v10-ui.yaml `assets` block)
- [ ] `Source/ui/public/js/juce/index.js` and `check_native_interop.js` both present (juce8-critical-patterns.md #13 - missing `check_native_interop.js` causes a silently frozen UI, not an error)

## 3. Update PluginEditor Files

- [ ] Adapt `v10-PluginEditor.h` into the real `Source/PluginEditor.h`. This file is a TEMPLATE - it makes several named ASSUMPTIONS about PluginProcessor's API (flagged inline with `// ASSUMPTION (flag for gui-agent)` comments) that MUST be verified/corrected against the real `PluginProcessor.h` produced by Stage 1-3:
  - `processorRef.getAPVTS()` - confirm this accessor name matches Stage 2's shell
  - `processorRef.getCustomStateTree()` - confirm the non-parameter ValueTree accessor name/shape (trigger mode, MIDI note, Chaos seed, custom tension curve - architecture.md State Persistence)
  - `processorRef.getLatestVisualizationSnapshot()` - confirm the ring-buffer drain accessor name/return shape (this template assumes a single struct-returning call; the real implementation may instead expose 5 separate atomic getters, matching CORRUPTR's per-field `getVisXxx()` precedent - either shape is fine, just keep `buildDropVisUpdateVar()` consistent with whichever is chosen)
- [ ] Verify member order: 19 relays -> webView -> 19 attachments (unchanged from the template - this is the load-bearing structural part, do not reorder)
- [ ] Adapt `v10-PluginEditor.cpp` to match, same ASSUMPTION call-sites
- [ ] Verify initialization order in the constructor matches declaration order

## 4. Wire the Non-APVTS "Trigger Mode" State

- [ ] `triggerMode` (manual/MIDI/auto-sync) is architecture.md non-parameter ValueTree state, NOT an APVTS parameter - confirmed no `WebComboBoxRelay`/`Attachment` pair exists for it in the template, by design
- [ ] Confirm Stage 2 Shell actually created a child ValueTree matching architecture.md's `<DropCustomState seed="..." triggerMode="midi" midiNote="60">...</DropCustomState>` shape (or equivalent) - adjust `getTriggerModeIndex()`/`setTriggerModeIndex()` in PluginEditor.cpp to match whatever property name/type Stage 2 actually used
- [ ] Test: clicking MANUAL/MIDI/AUTO in the Advanced page's trigger-mode pill persists across save/reload (plan.md Phase 4.2 test criterion)

## 5. Known Gap: MIDI Trigger Note Picker

- [ ] `architecture.md` lists a configurable MIDI trigger note as non-parameter state, and plan.md Phase 4.2 lists "MIDI-note-picker UI" as a goal - **but the finalized v10 mockup contains NO UI element for it** (not in v10-ui.yaml's `controls` list, not in v10-ui-test.html). This is the same category of gap CORRUPTR's own integration docs flag for concepts named in a creative brief but not carried into the finalized design.
- [ ] Resolution options (pick one, do not silently invent markup the design doesn't have):
  1. Ship Phase 4.2 without a note-picker UI (MIDI note defaults to a fixed constant, still configurable via saved-state XML/future preset editing) - lowest risk, matches "no HTML element exists to bind" precedent from other plugins in this repo
  2. Return to the ui-mockup skill for a small v11 iteration adding a note-picker control, then regenerate this integration set
  - Document whichever is chosen in `plugins/DROP/.ideas/architecture.md` or a Stage 3 GUI phase note.

## 6. Update CMakeLists.txt

- [ ] Merge `v10-CMakeLists.txt`'s WebView-specific sections (binary data target, WebView compile definitions, `NEEDS_WEB_BROWSER TRUE` / `NEEDS_WEBVIEW2 TRUE` / `NEEDS_MIDI_INPUT TRUE` plugin flags) into the real top-level CMakeLists.txt Stage 1 generates
- [ ] Verify `juce_add_binary_data(DROP_UIResources SOURCES ...)` includes all 4 files: `index.html`, `js/juce/index.js`, `js/juce/check_native_interop.js`, `v10-marble2-texture-hd.jpg`
- [ ] Verify `juce::juce_gui_extra` linked
- [ ] Verify `NEEDS_MIDI_INPUT TRUE` is present WITHOUT `IS_SYNTH TRUE` (DROP is an effect that also needs MIDI in, not an instrument - juce8-critical-patterns.md #22 is about the `IS_SYNTH` flag specifically, which does NOT apply here)

## 7. Bundle the Marble Asset (Detail)

- [ ] Confirm `v10-marble2-texture-hd.jpg` is the ACTIVE asset (v10-ui.yaml `assets` block explicitly marks `v10-marble-texture-hd.jpg` and `v10-stone-texture-hd.jpg` as kept-on-disk A/B alternates, NOT to be bundled)
- [ ] Confirm the resource-provider mapping in PluginEditor.cpp's `getResource()` matches the exact CMake-mangled BinaryData symbol: `v10-marble2-texture-hd.jpg` -> `BinaryData::v10_marble2_texture_hd_jpg` / `BinaryData::v10_marble2_texture_hd_jpgSize` (JUCE replaces every non-alphanumeric character with `_`) - run a build and check for a compile error on this exact symbol name as the fastest verification
- [ ] Test: the marble surface renders at full clarity (no filter/opacity/blend-mode - v10-ui.yaml explicitly validates this), scales proportionally on window resize, no tiling/swimming

## 8. Wire AudioAnalyzer -> Ring Buffer -> WebView Events (Stage 3 GUI Phase 4.3)

- [ ] Confirm Stage 3 DSP Phase 3.5 (IntensityBudget/DropStyle/AudioAnalyzer/Presets) has implemented the lock-free SPSC ring buffer described in architecture.md: `{ progress, tensionLevel, currentState, inputRMS, activeModuleWeights[10] }`, pushed once per processed block (or per sub-block segment) by the audio thread, drained on the message thread
- [ ] Implement/confirm the message-thread-safe drain accessor(s) on `DROPAudioProcessor` that `v10-PluginEditor.cpp`'s `buildDropVisUpdateVar()` calls (see Section 3's ASSUMPTION note)
- [ ] Verify `dropStateToString()`'s int->string mapping in PluginEditor.cpp stays in sync with `DropState`'s real enum order (`Idle=0, Arming=1, Building=2, Peak=3, Impact=4, Recovery=5` per architecture.md) - if DSP ever reorders the enum, this switch must be updated in lockstep
- [ ] Confirm `activeModuleWeights[10]`'s index order (this template assumes Filter/Width/Pitch/Gate/Stutter/Delay/Reverb/Distortion/Digital/Noise, matching the Processing Chain diagram - flagged as a best-guess assumption in PluginEditor.cpp, not a verified fact)
- [ ] Start the editor's `juce::Timer` at 30-60Hz (template uses `startTimerHz(30)`) and confirm `dropVisUpdate` events arrive in the WebView (`window.__JUCE__.backend.addEventListener('dropVisUpdate', ...)`, already wired in v10-ui.html)
- [ ] Test (plan.md Phase 4.3 criteria):
  - [ ] Progress strip glow tracks REAL DSP state (tension × inputRMS) during an active build, not a simulated/GUI-only animation. (NOTE 2026-09-14: the soap-bubble visualization was REMOVED from the finalized design at user direction — skip ALL bubble wiring; the progress strip + phase chip now carry the visualization contract. See v10-ui.yaml `bubble_removal`.)
  - [ ] Drop point and current transition position clearly visible (phaseChip + progressStrip)
  - [ ] Closing and reopening the editor mid-transition shows the CORRECT current state (verifies the UI reads processor state, never owns/originates it - v10-ui.html's trigger button no longer runs any local demo timeline, by design)
  - [ ] No CPU spikes from visualization (profile - Timer drain should be negligible vs. DSP cost)

## 9. Handle the IMPACT_VACUUM_FRACTION Approximation

- OBSOLETE (2026-09-14): this section applied to the removed bubble animation. With the bubble gone, no vacuum/burst visual split remains in the GUI; the phase chip simply displays the DSP's reported state. No action needed unless a future visualization reintroduces sub-phase visuals.

## 10. Test Parameter Binding (Phase 4.2)

- [ ] All 19 APVTS parameters sync UI <-> APVTS (trigger, buildLength, style, intensity, tension, chaos, impact, filter, width, pitch, gate, stutter, delay, reverb, distortion, digital, noise, dryWet, output)
- [ ] Host automation of all 19 parameters updates UI controls correctly
- [ ] DROP button press correctly fires `Idle -> Arming` and is a no-op while a build is already in flight (re-trigger safety, architecture.md) - verify the click-pulse pattern in v10-ui.html (`setValue(true)` then `setValue(false)` ~30ms later) produces a clean edge the DSP can detect; adjust the pulse timing/mechanism here if Stage 3 DSP's edge-detection needs differ from this assumption
- [ ] Automation gesture bracketing present on every interactive control (`sliderDragStarted`/`sliderDragEnded` - already wired in v10-ui.html per juce8-critical-patterns.md #23); spot-check: `grep -c "sliderDragStarted\|sliderDragEnded" Source/ui/public/index.html` should be well above 0
- [ ] Boolean `trigger` uses `getToggleState()`, not `getSliderState()` (juce8-critical-patterns.md #19 - already correct in v10-ui.html)
- [ ] No lag/visual glitches during rapid parameter changes

## 11. Build and Test

- [ ] Build succeeds without warnings (Debug)
- [ ] Standalone loads WebView (not blank); right-click -> Inspect shows no JavaScript errors; `window.__JUCE__` object exists
- [ ] Release build: no crashes on plugin reload (test 10+ times) - verifies member order correctness
- [ ] Test in a plain browser too (open `Source/ui/public/index.html` directly, or the original `.ideas/mockups/v10-ui-test.html` for full mock-backend behavior): confirms the graceful `window.__JUCE__` fallback path works (controls draggable via local state, no thrown errors, console shows the "running in browser preview mode" warning)
- [ ] Test in a DAW (per project convention, install to system plugin folders and restart the DAW - juce8-critical-patterns.md #10)

## Success Criteria

GUI integration is complete when:
- All 19 parameters sync UI <-> APVTS with correct automation/gesture behavior
- Trigger mode persists via non-parameter state
- The phase chip + progress strip (incl. tension×input glow) reflect REAL DropStateMachine state via the ring buffer, not any local/simulated timeline
- No release-build crashes on repeated plugin open/close (member order verified)
- Marble asset renders correctly and matches the finalized v10 design at all supported window sizes (800x450 .. 1400x790)
