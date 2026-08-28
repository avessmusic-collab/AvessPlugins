# KickDesigner2 — Reference Research Notes

**Method:** deep-research skill, Level 2 (local docs → web: MusicRadar, ModeAudio, Sonic Academy/Audiotent, JUCE forum + docs, MeldaProduction, SynthAnatomy, Pulsar Audio, ADSR, Screech House).
**Date:** 2026-08-28
**Status:** advisory input for `/plan` Stage 0. Not a contract.

---

## 1. Pitch envelope — "punch" not "laser"

**Core mechanism:** one envelope modulates oscillator pitch from a high start down to the fundamental. Punch response is governed by **two things above all: pitch-mod depth (start ratio) and envelope curve shape.**

- **Work in ratio/log domain**, never linear Hz. `f(t) = fundamental * startRatio^env(t)` where `env(t)` goes 1→0. Equivalent semitone form used by MinimalKick: `f = f0 * pow(2, envValue * semis/12)`.
- **Curve shape is the "laser" knob.** A sharp exponential *spike* that collapses in the first few ms = punch/click that cuts through a mix. A gentler/more-linear fall over a longer time = audible downward *sweep* ("zap"/"laser"). High bend + long time = laser; sharp bend + short time = thud.
- **909-style thud:** higher mod depth, *less* exponential (rounder) curve. **808-style:** lower depth, sharper/faster fall, less prominent punch.
- Envelope must be **phase-continuous** (integrate frequency into phase; never reset phase mid-fall) or you get clicks.

**Genre starting points (start ratio × fundamental, pitch time):**
| Genre | Fundamental | Pitch start | Pitch time | Curve |
|---|---|---|---|---|
| House / tech house | 45–55 Hz | 2–4× | 30–80 ms | moderately exp |
| Techno | 50–60 Hz | 3–5× | 20–60 ms | sharp exp |
| Hard techno | 55–65 Hz | 4–7× | 10–40 ms | very sharp exp |
| Hardstyle | 55–75 Hz (kick pitched to key) | 5–10× | **10–20 ms** fast drop then long tonal tail | very sharp; the "snap" is the first 10–20 ms only |
| Hardcore / gabber | 50–70 Hz | 6–10× | 8–25 ms | very sharp, then heavy distortion |
| Trap / DnB (808-ish) | 30–45 Hz | 2–3× | 40–120 ms | sharp fall, longer body |

**Implication for KD2 default (55 Hz, 4×, ~50 ms):** solid neutral techno/house punch. Keep default `pitchCurve` ~0.7 (toward exponential). Consider a subtle **two-stage** shape (very fast first 5–10 ms, then a slower settle) — several pro synths do this to get click + weight without a mid sweep.

---

## 2. Transient / click generator

**Reference architectures diverge here — and this is where KD2 differentiates:**
- **Sonic Academy Kick 2:** click = **sampled** layers (3 independent click layers, drag-drop samples, each with start/length/env/filter/pitch). Sub/body = clean sine + up to 7 addable harmonics. *KD2 spec forbids samples — so we synthesize the click instead.*
- **D16 PunchBox 2:** four parallel generators; three "universal" sample layers + one main kick engine (Sample/909/808/606/Wavetable). Multi-stage envelopes shape the sum.
- **Devious Machines Kick Ninja:** explicitly **synthesis-first** (no sample library reliance); marketed on low end that "holds its shape when pushed hard."

**Synthesized click recipe (algorithmic, per KD2 spec):** blend of
1. **Filtered noise burst** — white noise → bandpass (SVF / `dsp::StateVariableTPTFilter`) centred at `clickTone` (1–15 kHz), very short AD env (`clickTime` 0.1–50 ms, typ. 1–5 ms).
2. **Transient oscillator** — short high-frequency sine/triangle at `clickPitch` with its own fast pitch drop (adds tonal "tick" vs pure noise hiss).
3. **Impulse / filtered click** — single-sample impulse through a resonant lowpass, or a highpassed copy of the body's very first cycle. Gives the "snap" attack.

**Anti-aliasing:** noise bandwidth is inherently broadband — bandpass *before* any nonlinearity, and generate/shape the click **inside the oversampled section** if it feeds distortion. Keep the transient oscillator band-limited (wavetable sine, or `std::sin` on a phase accumulator is fine at these low levels/short durations). Avoid hard steps: window the impulse (short raised-cosine, ~4–16 samples) instead of a bare 1-sample spike.

---

## 3. Continuous distortion morphing

**Confirmed industry pattern (MeldaProduction MWaveShaper, Rawton Forge Deviation, DYNA PRO):**
- **Automatic gain compensation is standard** — "ensures the output sounds as loud as the input", RMS/loudness matching for honest A/B. KD2 must do this per-algorithm so a `character` sweep doesn't jump in loudness.
- Morphing is done by **A→B→C→D interpolation between slots** (MWaveShaper X-Y pad; DYNA "elastic morphing"). For KD2's single knob: place the 7 curves on a 1-D path and **crossfade adjacent pairs**.
- **Crossfade law matters:** equal-power vs equal-gain vs parabolic are all used in shipping products. For blending two *correlated* distorted signals of the same source, **equal-gain (linear) or a mild parabolic** is usually safer than equal-power (which can bump the middle).

**Proposed perceptual ordering (clean → destroyed):**
`bypass → tanh (soft, odd harmonics) → cubic soft-clip → asymmetric saturation (adds even harmonics) → soft-clip (poly/quintic) → hard-clip → foldback → bitcrush/decimate (digital)`
Rationale: monotonic-ish increase in harmonic density and "aggression"; analog-flavoured stages first, digital destruction last. `character` = position along this path; crossfade the two bracketing transfer functions.

**Gain compensation approach:** for each transfer function `f`, precompute an output-normalisation factor so that a reference sine (e.g. -12 dBFS, or a swept level) has matched RMS in vs out at a given `drive`. Simpler runtime alternative: measure short-window RMS pre/post and apply a smoothed makeup gain (what Melda/Deviation effectively do) — cheaper to get right than static tables, and adapts to `drive`.

**`drive`**: pre-gain into the shaper, up to ~+36 dB for "hard-techno destruction". **`driveMix`**: parallel dry/distorted blend (keeps sub intact under extreme settings). Order per hardstyle tutorials: **EQ/filter/tone-shaping BEFORE distortion** keeps it "tight and controlled under heavy drive" — consider letting Tone sit pre-distortion, or offer both.

---

## 4. Safety limiter that preserves the transient

**Findings:** a true transparent peak limiter needs **lookahead** (small delay, anticipates peaks → preserves attack). A **clipper** has zero latency but shaves the transient tip. Pro drum chains often **split the job**: transient shaper + limiter + final clipper as separate stages.

**Recommendation for KD2's *safety* limiter (goal = catch overs, not squash):**
- Short lookahead (~1–2 ms) soft-knee limiter with fast attack / medium release, threshold ~ -0.3 to -1.0 dBFS.
- Because it's a *safety* net that rarely engages on a well-designed kick, latency of 1–2 ms is acceptable **but must be reported to the host** (add to plugin latency alongside oversampling latency).
- Alternative zero-latency option: gentle `tanh`/soft-clip ceiling at -0.5 dBFS (no lookahead, no added latency) — arguably better fit for "don't destroy the transient" since the kick's peak *is* the point. **Consider making this the default** and reserving true lookahead limiting as an option.
- Do **not** let the limiter's release pump the tail — keep release short enough that it recovers before the body decay matters, or key it off peak only.

---

## 5. Sub / low-frequency mono & phase stability

- **Enforce mono below a crossover** (~120–150 Hz): `mid = 0.5*(L+R)` for the low band, apply width only to the high band, recombine. Do this in `OutputStage` after the layer sum, and also ensure the **sub oscillator is summed to both channels identically** (generate mono, copy to L/R).
- **Phase stability:** sub and body oscillators must **start from a defined phase on every trigger** (phase = 0, or a fixed offset) so stacked kicks and layered sub don't comb-filter. The pitch envelope changes frequency, not phase discontinuously.
- Body/sub relative phase: if body fundamental and sub are near the same frequency, a fixed phase relationship (e.g. both start at 0) avoids cancellation. Optionally expose a sub phase offset later, not in v1.
- DC blocker on the final output (asymmetric saturation + foldback generate DC offset).

---

## 6. JUCE `dsp::Oversampling` — latency & switching

**From JUCE docs + forum:**
- Constructor: `Oversampling(numChannels, factor, FilterType, isMaxQuality=true, useIntegerLatency=false)`. `FilterType`: `filterHalfBandPolyphaseIIR` (cheap, non-linear phase, **non-integer latency**) or `filterHalfBandFIREquiripple` (costlier, linear phase, integer latency).
- Flow: `initProcessing(maxBlockSize)` in `prepareToPlay`; `processSamplesUp(block)` → returns oversampled block → nonlinear processing → `processSamplesDown(block)`; `reset()` on transport reset.
- **Latency:** `getLatencyInSamples()` returns possibly **non-integer** latency. Plugins report integer latency to the DAW → either construct with `useIntegerLatency=true` (JUCE pads to integer internally) **or** round and compensate the fractional remainder yourself. Report via `setLatencySamples(round(os.getLatencyInSamples()) + limiterLookaheadSamples)`.
- **Runtime factor change is NOT directly supported.** Docs only describe construction. Established pattern:
  1. Pre-construct **one `Oversampling` object per factor** (1/2/4/8×) at `prepareToPlay`, all `initProcessing`'d, OR
  2. Keep a `std::unique_ptr<Oversampling>` and rebuild it — but **never rebuild on the audio thread**. Do it on the message thread (parameter listener), guard the swap with an atomic pointer / `try_lock` in `processBlock` falling back to previous, and **fade out around the switch** (1 short block ramp to silence, swap, ramp back) to avoid a glitch.
  3. On any switch: call `setLatencySamples(...)` with the new total and `reset()` the new object. Expect a DAW plugin-delay-compensation re-scan (brief audio drop in some hosts — acceptable, and why fade is used).
- 1× ("off") = use `addDummyOversamplingStage()` / factor 1 path so the signal path stays uniform.
- **Default:** ship 2× while profiling; move to 4× only if CPU headroom is comfortable (kick is bursty/monophonic so 4× is likely fine). Nonlinear stages (distortion, tail drive, body saturation, click-into-distortion) go inside; linear stages (tone EQ, stereo, limiter) stay outside to save CPU.

---

## 7. Reference plugins — what makes them commercial-grade

| Plugin | Engine | Notable |
|---|---|---|
| **Sonic Academy Kick 2** | clean sine + up to 7 harmonics (sub/body) + **3 sampled click layers** | "cleaner synth engine, less aliasing" is a headline selling point; real-time waveform; huge preset library; per-layer envelope/filter/pitch |
| **D16 PunchBox 2** | 4 parallel generators: 3 universal (sample) layers + main engine (Sample/909/808/606/**Wavetable**); multi-stage envelopes | blends multiple synthesis methods into one kick; deep envelope editing; genre preset packs |
| **Devious Machines Kick Ninja** | **synthesis-first**, no sample-library reliance | "low end holds its shape when pushed hard"; positioned against sample-based competitors — closest philosophical match to KD2 |
| **Initial Audio Dynamic Drum Sampler / 808 Studio** | sample + synth hybrid, heavy on shaping/FX | strong distortion & shaping chain, good preset curation |

**Common commercial-grade traits to match:**
1. **Clean engine / low aliasing** — oversampled nonlinearities, band-limited oscillators. Kick 2 literally markets this.
2. **Large, genre-organised, original preset library** with a fast browser (prev/next, categories).
3. **Real-time waveform display** of the current kick — every top plugin has it; it's a core UX expectation.
4. **Per-layer envelopes & pitch** — independent control of each component.
5. **Deep but fast** — macro/one-knob shaping on top of full editability. KD2's Punch/Body/Crush/Tail macros cover this.
6. **Distortion that scales** from subtle saturation to total destruction, gain-compensated.
7. **Rock-solid state/preset recall and automation.**

**KD2's differentiation angle:** fully algorithmic (no samples at all, including clicks), morphing distortion on a single knob, constrained musical Randomize/Mutate, native low-latency analyzer.

---

## Open questions for `/plan` Stage 0

1. Confirm **default oversampling** target after CPU profiling (2× vs 4×).
2. Decide **safety limiter design**: zero-latency soft-clip ceiling (default, no added latency) vs short-lookahead true limiter (option, +1–2 ms reported latency).
3. Lock the **distortion morph ordering** and choose **static gain tables vs adaptive RMS makeup** for compensation.
4. **Tone position:** pre-distortion, post-distortion, or switchable.
5. Pitch envelope: single exponential vs **two-stage** (fast snap + settle) for the default punch.
6. Low/high **crossover frequency** for mono enforcement (~120–150 Hz) — fixed or exposed.
7. Click generator: exact blend weights of noise/osc/impulse and whether click is generated inside the oversampled block.

## Sources

- https://www.musicradar.com/how-to/how-to-create-a-kick-drum-using-a-vst-plugin-soft-synth
- https://modeaudio.com/magazine/drum-synth-sound-design-kick-snare
- https://www.musicradar.com/tuition/tech/how-to-synthesize-a-kick-drum-in-nis-massive-639049
- https://www.musicradar.com/reviews/tech/sonic-academy-kick-2-640392
- https://www.audiotent.com/blogs/production-tips/production-tipsultimate-guide-sonic-academy-kick-2
- https://www.musicradar.com/how-to/how-to-synthesise-a-house-kick-using-sonic-academy-kick-2
- https://synthanatomy.com/2026/06/d16-group-punchbox-2-kick-drum-synth-plugin-gets-a-powerful-makeover.html
- https://www.kvraudio.com/product/punchbox-2-by-d16-group
- https://pluginerds.com/7-best-kick-plugins/
- https://docs.juce.com/master/classdsp_1_1Oversampling.html
- https://forum.juce.com/t/dsp-oversampling-setup/46896
- https://forum.juce.com/t/help-with-dsp-oversampling/50163
- https://www.meldaproduction.com/MWaveShaper/features
- https://isotonikstudios.com/product/deviation-by-rawton-forge/
- https://sonicstate.com/news/2024/12/09/morphing-waveshaper-distortion-plug-ins/
- https://pulsar.audio/ipa25-clipper-limiter/
- https://mixelite.com/blog/compression-vs-clipping-vs-limiting/
- https://www.adsrsounds.com/serum-tutorials/hardstyle-kick-sound-design-with-serum/
- https://futureproofmusicschool.com/blog/making-hard-techno-a-path-to-unique-sound-design
- https://screechhouse.com/how-to-start-making-a-hardstyle-kick-best-hardstyle-kick-sound-design-tricks-fl-studio-hardstyle/
