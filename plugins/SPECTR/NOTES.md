# SPECTR Notes (formerly PRISM)

## Status
- **Current Status:** 📦 Installed
- **Version:** 1.0.0
- **Type:** Audio Effect (Multi-Source Analyzer/Visualizer)

## Lifecycle Timeline

- **2026-11-09 (master CH switch + review pass + v1.0.0 committed):** Readout CH1/CH2 chips became a MASTER source switch (readouts + needle + all four panes follow; per-pane chips still override). Scoped /code-review (plugins/SPECTR): 5 findings fixed — wfIngest empty-batch no longer cancels the synthetic-tail re-base (waveform froze ~3 s after host suspend), per-source loudness fields reset in resetSourceScalars (pooled-buffer invariant), StereoMetrics reset on ring-overrun gaps, single peak scan in finishColumn, stale PRISM dist artifacts dropped. Suite 571/571, pluginval S5 SUCCESS, installed; PKG regenerated and copied to ~/Desktop/MY VST.

- **2026-11-04 (hover info):** Custom tooltip system added (single #tip div inside #stage, delegated pointerover/move/out, 450 ms rest delay, native title attrs migrated to data-tip). ~30 targets incl. readout-cell metering explanations. Page JS syntax-verified (new Function compile); suite 571/571; rebuilt + installed. VISUAL CHECK PENDING — screen was locked during the session (windows cannot order onscreen on a locked screen; System Events reports 0 windows — same class as the known "HiDPI retest on unlocked screen" item).
- **2026-11-04 (fullscreen-in-DAW fix):** First Ableton test caught the kiosk fullscreen being blocked by the vacated always-on-top plugin editor window (gray, floating above). Fix: kiosk window setAlwaysOnTop(true) + the vacated editor paints an "in true fullscreen, Esc/F to return" hint. Rebuilt + reinstalled; suite 571/571. Lesson: DAW plugin windows float at an elevated window level — any plugin-spawned top-level window (kiosk, popout) must be always-on-top or it lands BENEATH the editor it vacated; Standalone cannot catch this class of bug.
- **2026-11-03 (SPECTR 1.0.0 — rebrand + readout selector):** Full rename PRISM → SPECTR (PLUGIN_CODE Prsm → Sptr, bridge events prism* → spectr*, folder, CI workflow, Inno installer; old PRISM VST3/AU uninstalled — breaking, accepted pre-release; rollback at backups/PRISM/v1.0.0/). Default theme → Cyan/Violet (param default index 1 + page boot tokens). NEW: CH1/CH2 chips in the nameplate pane switch the whole readout column (Loudness/TP/Stereo) + needle meter to one source — per-source loudness now published in SourceSnapshot + meters.louds[] (each source's LoudnessMeter was already always-on); persisted as ui_readout_source. Waveform: double-click resets zoom to 8 bars. Suite 571/571; pluginval S5 SUCCESS; cross-format identity + seals OK. MANUAL DAW TESTS PENDING (rename smoke test in Live + inherited §10 checklist). Windows installer/CI renamed but NOT re-run since rename.

- **2026-10-22 (Windows 1.0.0):** Windows CI pipeline green (run 39745658151): VST3 + test exe build (WebView2 static loader via JUCE FindWebView2 - package location must be the NuGet PARENT dir), full suite 565/565 on Windows (64MB /STACK; 3 timing races fixed incl. ack-seam fetch race), pluginval S5 SUCCESS (gate on printed verdict - pluginval.exe is GUI-subsystem, pwsh does not wait), Inno Setup installer built + archived to dist/. macOS install verified current (binaries 10-18 = latest product code) and standalone UI verified 60fps. Windows installer NOT yet smoke-tested on a real Windows machine (GUI tests skipped on headless CI).
- **2026-10-16:** Full QA sweep: code review (8 findings, all fixed incl. grid re-base displacement, Reset column duplication, LRA cross-gap blending), pluginval S5 SUCCESS, ASan/UBSan 0 reports, TSAN 0 races, suite 509/509 x5 idle (all intermittents proven load-flakes). Waveform refined through 8 user rounds: CDJ nested-rim layers, bar-based spans 1-32, unified sample-time grid (worst error ~11us, zero drift), onset alignment w/ filter-delay compensation, live-edge column (~19ms modeled latency), quiet grid layer, Ch1/Ch2 on all panes.
- **2026-10-14:** Creative brief finalized via /dream (plugin-ideation)
- **2026-10-15 (v1.0.0, Stage 3 COMPLETE — plugin working & installed):** Real v8 UI embedded and live-data wired. Verified: tone-accurate displays (LUFS −12.03 on −12dBFS sine, TP/SP −12.04, corr +1.00, mono = vertical gonio M-line), frame push p99 1.32ms (<2ms gate), multi-instance isolation, host-chunk reload, reset semantics, 26-min flat soak, 463 tests green. MANUAL DAW TESTS PENDING — checklist in .ideas/mockups/v8-integration-checklist.md §10 (DAW matrix, automation feel, freeze visual / fullscreen persistence / HiDPI retest on unlocked screen, 1-hour soak, WebView CPU gate in visible session).
- **2026-10-15 (Stage 2 COMPLETE):** Full analysis engine, 7 phases, 74 test cases / 448 checks green. QA: pluginval strictness 5 PASS (live engine); ASan/UBSan clean (needs 64MB stack — sanitizer frame inflation); TSAN clean (0 race warnings; its real-time-budget failures are 20x instrumentation slowdown, not defects). Loudness verified vs libebur128+pyloudnorm goldens. Reset semantics: applied at worker time (≤10ms in-flight audio counts as post-reset; sample peak exact via lossless atomic).
  **Worker CPU profile** (pump+snapshot, % of one core, Release): 48k 1src closed 0.18 / open 0.37; 48k 2src closed 0.37 / open 0.73; 96k 2src open 1.44; 192k 2src open 2.84 (worst), closed 1.55; FFT16384@192k spectrum path 3.15. Memory: 6.92MB (2 sources, default rings, 4 live snapshots). All budgets met (open <3%@48k, <8%@192k, closed <0.5%@48k, mem <10MB).
- **2026-10-15 (Stage 1):** Foundation + shell complete — CMake (VST3/AU/Standalone, WebView-ready), 17 APVTS params zero-drift, dual-bus, bit-transparent passthrough, state sanitizer, snapshot mailbox stubs. Build 82s; pluginval strictness 5 PASS.
- **2026-10-15 (Stage 0):** architecture.md + plan.md written (complexity 5.0/Tier 6; lock-free ring → worker analysis → snapshot publishing; custom K-weighting + 4x polyphase true peak; phased Stage 2/3 plan with EBU/libebur128 golden tests). Status → 🚧 Stage 0.
- **2026-10-15:** QA pass: numerical metrics audit (vs SPAN/MiniMeters conventions + BS.1770-5/R128) + high-effort code review. 1 critical fix (goniometer L/R mirror), spectrogram de-tilted, meter formula definitions locked into spec/YAML, C++ editor hardened (hidden-view sync, state-recall resync, gesture protocol, 192kHz scope buffer, split sanitizing). All display math verified correct (dB/log-axis/notes/correlation/LUFS/2-decimal cap/V-H reprojection).
- **2026-10-15:** Spectrogram V/H orientation toggle added (UI state, not a parameter); spectrogram pane min width 224px
- **2026-10-15:** UI restyled to v8 per user's scientific-instrument reference (edge-to-edge bordered pane grid, right sidebar with needle meter + knobs); v8 finalized, implementation files regenerated (parameter contract unchanged: 17 APVTS + 2 state)
- **2026-10-15:** UI mockup finalized at v7 (7 iterations: Studio Hardware → glass-device redesign from user reference, 7-theme system, waterfall spectrogram, 3-band beat waveform, drag-resize + fullscreen). Implementation files + locked parameter-spec.md generated (17 APVTS params + 2 state entries).

## Known Issues

None

## Additional Notes

Premium real-time audio visualization plugin: multi-source spectrum analyzer (juce::dsp::FFT, 1024–16384, 75% overlap), BS.1770-5/R128 loudness + true-peak metering, modular dashboard (spectrum, spectrogram, vectorscope, oscilloscope, LUFS/correlation/width/peak-RMS meters, waveform history). Bit-transparent passthrough, zero latency, strict lock-free real-time architecture.

v1.0 scope: 2 stereo sources (main + sidechain — Ableton Live's routing ceiling), all visual modules, WebView UI, VST3 + AU. v2: PRISM Sender companion plugin + shared-memory IPC for true 8–16 source operation in Live.

Full vision: `plugins/PRISM/.ideas/creative-brief.md`
- **Last Packaged:** 2026-11-09
- **Distribution:** plugins/SPECTR/dist/SPECTR.pkg (3.5 MB, macOS; also copied to ~/Desktop/MY VST). Windows installer pending a CI re-run of spectr-windows.yml (old PRISM exe removed).
