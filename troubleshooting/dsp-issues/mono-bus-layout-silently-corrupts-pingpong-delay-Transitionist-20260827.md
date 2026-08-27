---
plugin: Transitionist
date: 2026-08-27
problem_type: dsp_processing
component: delay
symptoms:
  - "No crash and no pluginval failure, but a code comment claims a channel-count guarantee that nothing in the code actually enforces"
  - "processBlock() aliases its left/right write pointers to the same buffer when hosted with a mono bus layout"
root_cause: incorrect_assumption
juce_version: 8.x
resolution_type: code_fix
severity: medium
tags: [dsp, buses, mono, delay, latent-bug]
---

# Missing `isBusesLayoutSupported()` Override Lets Hosts Negotiate a Mono Bus, Silently Breaking the Ping-Pong Delay

## Problem

Found during a general bug audit (not from a user-reported symptom). In
`Source/PluginProcessor.cpp`, the ping-pong delay's per-sample loop reads:

```cpp
// numChannels is guaranteed >= 2 by this plugin's stereo-only bus config.
auto* left = buffer.getWritePointer(0);
auto* right = numChannels > 1 ? buffer.getWritePointer(1) : buffer.getWritePointer(0);
```

The comment claims `numChannels >= 2` is guaranteed by the plugin's bus
configuration. Nothing enforced that guarantee: `TransitionistAudioProcessor`
never overrode `isBusesLayoutSupported()`, and JUCE's default implementation
accepts any layout a host proposes. Some hosts negotiate a mono-in/mono-out
layout for an effect inserted on a mono track.

## Investigation

- Confirmed via `pluginval`'s "Listing available buses" test (strictness
  level 5) that, prior to any fix, the plugin reported accepting Mono, LCR,
  Quadraphonic, 5.0/5.1/7.0/7.1 Surround, and Discrete layouts in addition
  to Stereo - i.e., genuinely unrestricted, not just a cosmetic listing.
- Traced what happens if `numChannels == 1`: `left` and `right` become the
  SAME pointer (`buffer.getWritePointer(0)` called twice). In the per-sample
  loop:
  ```cpp
  left[n]  = left[n]  * (1.0f - delayNorm) + delayedA * delayNorm;
  right[n] = right[n] * (1.0f - delayNorm) + delayedB * delayNorm;
  ```
  the second assignment writes to the exact same memory location as the
  first, unconditionally overwriting it. The `delayedA`/left-channel
  contribution is computed and then immediately discarded every sample -
  not a crash, but a silent, unpredictable degradation of the ping-pong
  delay's intended alternating character whenever the host uses a mono bus.
- Cross-checked the rest of the codebase: `DrumRoulette` already has an
  explicit `isBusesLayoutSupported()` override (for a different, multi-output
  reason); no other plugin was audited during this pass, but the omission
  here was clearly unintentional given the comment's own claim.
- `pluginval --strictness-level 5` (Automation, Audio processing, Plugin
  state, etc.) passed both before and after this fix - this class of bug
  does not produce NaN/Inf/crashes, so pluginval alone cannot catch it.

## Root Cause

Missing `AudioProcessor::isBusesLayoutSupported()` override. JUCE's default
behavior is permissive (accepts any proposed layout) unless a subclass
restricts it - this plugin's code assumed a restriction that was never
actually implemented.

## Solution

Added an explicit override enforcing stereo-in/stereo-out only, matching
what the rest of the DSP code already assumes:

```cpp
// PluginProcessor.h
bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

// PluginProcessor.cpp
bool TransitionistAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}
```

This makes the existing "numChannels is guaranteed >= 2" comment actually
true, rather than an unenforced assumption - closing the bug by
construction instead of adding a runtime branch for the mono case.

## Verification

Re-ran `pluginval --strictness-level 5` against the rebuilt VST3:
"Listing available buses" now reports only `Stereo` for both input and
output (previously listed 8 layout families). All other test categories
(Audio processing, Automation, Plugin state, Basic bus, etc.) still SUCCESS.

## Prevention

**Detection heuristic:** any comment in `processBlock()` (or elsewhere)
asserting a channel-count/layout guarantee ("numChannels is guaranteed to
be N", "this is always stereo", etc.) must be backed by an actual
`isBusesLayoutSupported()` override - grep for the claim, then verify the
enforcement exists, don't assume BusesProperties construction alone
restricts what hosts can negotiate at runtime.

**For this codebase generally:** worth auditing other WebView/DSP plugins
built with a fixed stereo-only `BusesProperties()` constructor call for the
same gap - the constructor only sets the *default/preferred* layout, it does
not by itself restrict what `isBusesLayoutSupported()` will accept.

## Related Issues

None currently on file for this specific gap in other plugins - flagged in
session notes as worth a follow-up audit.
