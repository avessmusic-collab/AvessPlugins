# CHOPLINE — Third-Party Licenses

Every dependency must be MIT, BSD, zlib, Apache-2.0 or the JUCE licence (spec §2).
No GPL/LGPL code anywhere in the project.

## In use

| Dependency | Version | License | Notes |
| --- | --- | --- | --- |
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.4 (pinned tag, FetchContent) | JUCE licence (dual: JUCE EULA / AGPLv3) | Used under the JUCE licence terms, **not** AGPL. Personal/Indie tier applies under the revenue cap — **verify the current JUCE 9 revenue-cap tier and buy the appropriate licence before any public/commercial release.** Splash/attribution requirements depend on tier. |
| [Catch2](https://github.com/catchorg/Catch2) | v3.16.1 (pinned tag, FetchContent) | BSL-1.0 | Test-only dependency; not linked into the shipped plugin. |
| [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | 1.4.0 (pinned tag, FetchContent) | MIT | Header-only, real-time time-stretch/pitch-shift with formant preservation (Phase 3 warp engine: Stretch warp mode + Fit-step playback). © Geraint Luff / Signalsmith Audio Ltd. |
| [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear) | 0.6.4 (pinned transitively by signalsmith-stretch 1.4.0's own CMake) | MIT | FFT/STFT library used by Signalsmith Stretch. © Signalsmith Audio. |

## Explicitly not allowed

- Rubber Band Library (GPL unless a commercial licence is bought) — spec §2.
- Any GPL/LGPL dependency.
