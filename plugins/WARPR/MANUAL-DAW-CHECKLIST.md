# VOID - manual DAW checklist (1.0.0)

Machine tests cannot open a real host. Work through this on the release build installed with the repository install script
(restart the DAW afterwards). Tick = pass; write the DAW version and what you saw for any failure.

Tester: ____________   Date: ____________   Build: ____________   macOS: ____________

## A. Ableton Live (primary)  - Live version: ________

### A1. Load and basics
- [ ] Rescan: VOID appears as an **instrument** (VST3 and, if used, AU). It is not listed as an effect.
- [ ] Drop it on a MIDI track: the editor opens, is not blank, and the default wave is drawn.
- [ ] Play notes from a MIDI clip and from the QWERTY / MIDI keyboard: sound, no clicks on note on / off.
- [ ] Open and close the editor 10 times (also with the device collapsed): no crash, no blank window, parameters unchanged.
- [ ] Resize the editor (small and large): uniform scale, no clipped controls.
- [ ] Two instances on two tracks: independent sounds, no cross talk, both save and reload (section A6).

### A2. Drop a sample
- [ ] Drag a WAV from Live's browser onto the waveform. If Live / the web view swallows the drop, use **LOAD**: the file loads.
- [ ] Load a mono sample and a stereo sample; a 24-bit one; a very short one (< 100 ms) and a long one (> 30 s): no hang.
- [ ] The waveform, playheads and the processed-wave preview update; changing warp amount moves the preview live while dragging.
- [ ] Load a second sample while a note is held: the held note finishes cleanly, new notes use the new sample.

### A3. One-Shot trigger mode
- [ ] Loop mode One-Shot: tap a key briefly: the **whole** sample plays out (key-up does not cut it).
- [ ] Hold the sustain pedal / release it: no change in a one-shot (it already plays to the end).
- [ ] Retrigger quickly: each tap restarts the sample without a click (Poly: a new voice; Mono: restarts through a short fade).
- [ ] Switch Loop mode back to Forward Loop while a one-shot sounds: it can then be released by key-up.
- [ ] Granular play mode with One-Shot: behaves as gated (grains have no natural end).

### A4. Automation
- [ ] Show automation for VOID: about 115 parameters are listed (the 20 LFO grid selectors are hidden); names are readable.
- [ ] Automate **Warp Amount**: draw a ramp; the sound follows and the knob moves in the UI; no zipper noise at slow ramps.
- [ ] Automate **Filter Cutoff** (sweep 20 Hz - 20 kHz): smooth, no clicks; with resonance up there is no blow-up.
- [ ] Automate **Warp Mode** (a stepped lane): switches crossfade, no hard clicks.
- [ ] Record automation by turning knobs in the UI with Live armed for automation: the lane captures the gesture (touch / release).
- [ ] Automate **Oversampling**: sounding notes keep their factor, the next note uses the new one (no click).

### A5. Presets
- [ ] Preset bar: the factory preset **Init** loads and resets every control, the sample (default wave) and the LFO / shaper UI state.
- [ ] Save a user preset with a loaded sample, a custom shaper curve and a modulated LFO route; change everything; load it again: identical sound.
- [ ] Step through presets with the arrows (needs >= 2 saved): wraps around, name shown.
- [ ] A preset saved with a sample opens on a machine / folder where the original file no longer exists (the sample is embedded).

### A6. Project persistence
- [ ] Build a patch (sample, warp, shaper curve, filter, 2 LFO routes), save the Live set, close Live, reopen: the editor shows the same patch; the sound is identical.
- [ ] Duplicate the track: the copy sounds identical. Freeze / flatten the track: renders without errors.
- [ ] Save as a Live device preset (.adv), reload it: same as above.
- [ ] Reopen with the plugin removed from the scan and back: no crash.

### A7. Limiter and gain staging
- [ ] Master Gain +12 dB with a loud patch (warp distortion at max): the output peaks at **-0.1 dBFS and never clips** on Live's meter; no pumping artefacts beyond the intended limiting.
- [ ] Quiet material is untouched (compare with the limiter engaged and bypassed by lowering gain: levels below the ceiling are bit-exact, so no level change).
- [ ] Sustained clipping then silence: the gain recovers within about 100 ms (no stuck reduction).
- [ ] Export / bounce a loud passage: the file's peak is <= -0.1 dBFS, no NaN / silence glitches.

### A8. Settings and CPU
- [ ] Open Settings from the UI: oscillator budget, Interpolation (High / Eco) and CPU guard are shown with their stored values.
- [ ] Switch Interpolation to Eco during playback: the sound changes slightly (duller top end at transposed pitches), no click or crash; back to High restores it.
- [ ] Heavy patch (16 voices, unison 8, 8x): Live's CPU meter reacts; with the budget at 256 the patch still plays (new notes lose unison first).
- [ ] Turn the CPU guard on and overload on purpose (many notes at 8x): there are no dropouts that were not there before, new notes sound slightly more aliased, and **sounding notes do not change**. Turn it off: new notes are full quality again.
- [ ] Typical patch (8 voices, unison 2, 2x, warp + shaper + filter, 2 LFO routes): Live's CPU well under 15 % of one core. Note the figure: ______ %
- [ ] Change the audio buffer size and sample rate (44.1 / 48 / 96 kHz) with a patch loaded: no crash, pitch is correct afterwards.

### A9. Edge cases
- [ ] Panic: all-notes-off / stop with a held note: voices release; no stuck notes after a long session.
- [ ] Pitch bend and mod wheel from a controller: bend follows the Bend Range; the mod wheel does nothing (documented).
- [ ] Mono / Legato voice modes with glide: no clicks on legato overlaps.
- [ ] Host tempo sync: an LFO in Sync mode locks to the Live tempo; change the tempo during playback: it follows; loop the clip: it stays locked.

## B. Logic Pro (AU)  - Logic version: ________

- [ ] The AU passes validation (`auval -v aumu Void Vstf`) and VOID is listed under the instrument slot (software instrument).
- [ ] Software Instrument track: editor opens (not blank), plays notes, default wave audible.
- [ ] Drop / LOAD a sample; save the project; reopen: patch and sample restored.
- [ ] Automate a parameter in Logic's automation lane (Warp Amount): follows in both directions.
- [ ] Save a Logic channel strip setting; recall it on a new track: same sound.
- [ ] Bounce in place a loud passage: peak <= -0.1 dBFS.
- [ ] Open / close the editor 10 times: no crash.

## C. Reaper (VST3)  - Reaper version: ________

- [ ] Insert VOID on a track (as an instrument via the FX window with MIDI input): plays, the editor opens.
- [ ] Parameter modulation / envelope on Warp Amount through Reaper's "Parameter modulation": works.
- [ ] Save the project, reopen: patch restored. Save an FX chain, reload it: same.
- [ ] Render the project to a file (offline): sound identical to realtime playback, no silence at the start.
- [ ] Open / close the FX window 10 times: no crash.

## D. Release-candidate machine checks (orchestrator)

- [ ] `WARPR_Tests` quick and `--full` all pass, zero warnings in a clean build of VST3, AU, Standalone and WARPR_Tests.
- [ ] `WARPR_Tests --bench` run on the release build; the table is pasted into CHANGELOG.md; the typical patch is < 15 %.
- [ ] TSAN and ASAN / UBSAN builds of WARPR_Tests pass.
- [ ] pluginval strictness 5 on the VST3: success. `auval` on the AU: pass.
- [ ] Windows: build + pluginval on the CI machine (deferred item, see CHANGELOG).

Result: [ ] ship   [ ] fix first   Notes:
