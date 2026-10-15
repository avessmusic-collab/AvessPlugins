# PRISM Notes

## Status
- **Current Status:** 📦 Installed
- **Version:** 1.0.0
- **Type:** Audio Effect (Multi-Source Analyzer/Visualizer)

## Lifecycle Timeline

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
