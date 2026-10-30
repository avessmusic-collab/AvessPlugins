# TENSION Notes

## Status
- **Current Status:** 📦 Installed
- **Version:** 1.0.0
- **Type:** Audio Effect (One-Knob Cinematic Riser)

## Lifecycle Timeline

- **2026-10-29:** Ideated from a DROP "remake as one knob" session; user supplied a full authoritative build spec (`.ideas/implementation-spec.md`).
- **2026-10-29 (Stage 0):** Research & planning — architecture.md + plan.md (Tier 6, phased).
- **2026-10-29 (Stage 1):** Foundation + shell — 5 APVTS params, VST3+AU+Standalone, native GUI (no WebView). Root-build CHOPLINE collision fixed factory-wide.
- **2026-10-29/30 (Phases 3.1–3.6):** Full DSP: engine + OutputSafety, series modules, noise + sends, real tension mapping + rise modes, tuning + metrics harness, robustness. 331-check suite, TENSION_Profiles harness, 3 real bugs found/fixed (delay slew reversal, rate-dependent noise level, reverb re-prepare state).
- **2026-10-29 (UI design):** Mockups v1→v4; v4 finalized (3D knobs, bezel, per-mode themes, Cinematic teal→blue).
- **2026-10-30 (Phases 4.1–4.3):** Native JUCE GUI complete; visualizer live from telemetry; pluginval strictness 10 with GUI tests SUCCESS.
- **2026-10-30 (1.0.0):** Installed to system folders.

## Known Issues

None blocking. Pink-noise spectral tilt varies mildly with host rate (documented, v1-accepted). Manual DAW checklist pending (see CHANGELOG).

## Additional Notes

- Authoritative contract: `.ideas/implementation-spec.md`; design contract: `.ideas/mockups/v4-ui.yaml` (finalized).
- Key invariant: 0% tension is bit-exact passthrough (delta-form routing + exact-zero snapping + tail trackers).
- Test targets: `TENSION_Tests` (331 checks), `TENSION_Profiles` (ramp metrics), `TENSION_UISnapshot` (offscreen editor PNG: `./build/plugins/TENSION/TENSION_UISnapshot out.png [tension%] [riseMode 0-2] [animSec]`).
- CPU: ~1.1% of one core @48 kHz stereo. Latency 0. Tail reported 8 s.
- User decisions locked: native GUI, noise inside wet bus (MIX 0% = dry), mono→mono/stereo→stereo.
