---
plugin: Transitionist
date: 2026-08-27
problem_type: ui_layout
component: webview
symptoms:
  - "Standalone app intermittently shows a blank white screen on launch, requiring a relaunch to see the real UI"
  - "Newly-added JS/HTML changes sometimes appear not to run at all, even though the compiled binary (confirmed via `strings`) contains the new code"
  - "Visible parameter values/knob positions look correct even when a fresh diagnostic marker embedded in the same HTML does not appear"
root_cause: api_misuse
juce_version: 8.x
resolution_type: code_fix
severity: high
tags: [webview, cache, wkwebview, standalone, gui]
---

# WKWebView Restoring a Stale Cached Page Snapshot Across App Relaunches

## Problem

While debugging an unrelated preset-dropdown rendering bug, added a unique
text marker to `index.html`'s logo (`TRANSITIONIST [BUILD-CACHE-TEST-42]`)
to sanity-check that the running Standalone app was actually executing the
current build. Confirmed via `strings` on the compiled binary that the
marker WAS present. Despite this, several consecutive rebuild+relaunch
cycles showed:
- The FIRST launch after each rebuild: blank white screen (no content at all)
- A RELAUNCH (quit + reopen, same binary): correctly showed the marker text
  AND correct current parameter values - but new diagnostic code added in
  the SAME rebuild (a `try/catch` block designed to mutate the logo text to
  prove a specific code path executed) never visibly ran, across many
  consecutive rebuild/relaunch cycles, even though `strings` confirmed the
  new code was present in the binary each time.

This combination (some content updates correctly, others silently do not,
despite both being compiled into the same binary and loaded via the same
resource provider) pointed to the WebView not doing a genuinely fresh
execution of the page on every launch.

## Investigation

Ruled out (each confirmed via a targeted test):
1. **JS syntax errors** - script (minus the ES module `import` line, which
   `osascript -l JavaScript`/JavaScriptCore doesn't support) parsed and
   executed without error via `osascript -l JavaScript -e "..."`.
2. **Total script execution failure** - other JS-driven UI (knob positions,
   slider fill height, LCD text) rendered correctly, proving the script DID
   execute far enough to update the DOM in most places.
3. **BinaryData/build staleness** - `strings` on the compiled binary
   confirmed every new marker string was genuinely present after each
   rebuild (this was the OTHER stale-build issue from an earlier session,
   already fixed - see
   `troubleshooting/build-failures/standalone-target-never-rebuilt-by-build-script-Transitionist-20260826.md` -
   and re-confirmed NOT to be the cause here).
4. **CSS-only rendering quirk** - ruled out because even a JS statement as
   simple as `document.querySelector('.logo').textContent = 'X'` (no
   `<select>`/DOM-creation complexity at all) failed to visibly take effect
   on "successful-looking" relaunches.

## Root Cause

WKWebView (macOS's WebKit-based web engine, used by JUCE's
`WebBrowserComponent`) can restore a **cached/back-forward-cache-style
snapshot** of a previously-loaded page for a given URL, rather than
re-fetching and re-executing it fresh, when the exact same URL is navigated
to again. Because this plugin (like all WebView plugins in this codebase)
always calls:

```cpp
webView->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
```

...the URL is **identical on every single launch**, across every app
relaunch, even after a full rebuild with completely different HTML/JS/CSS
content. WebKit has no way to know the content behind that URL changed
(there's no HTTP-style cache-control/ETag mechanism at play for a custom
resource-provider scheme), so on some launches it appears to substitute a
previously-rendered/cached version of the page instead of doing a true
fresh load - explaining both the intermittent blank-white-screen launches
(a genuine fresh-load race/failure) AND the "some new code doesn't seem to
run" confusion (a stale snapshot from an earlier successful load being
reused, with its DOM already in whatever state it was in when captured -
including whichever knob/slider values happened to be set at that time).

## Solution

Append a unique, per-instance query string to the URL passed to
`goToURL()`, forcing WebKit to treat every launch as a **distinct URL** it
cannot have cached:

```cpp
// PluginEditor.cpp constructor
const juce::String cacheBuster = "?t=" + juce::String(juce::Time::getMillisecondCounterHiRes(), 0);
webView->goToURL(juce::WebBrowserComponent::getResourceProviderRoot() + cacheBuster);
```

`getResource()` must strip the query string before matching against known
paths, since the resource provider otherwise won't recognize
`"/?t=12345"` as `"/"`:

```cpp
std::optional<juce::WebBrowserComponent::Resource> TransitionistAudioProcessorEditor::getResource(
    const juce::String& url)
{
    // Strip any query string before matching - it exists purely to give
    // WebKit a unique URL per launch, not to select a different resource.
    const juce::String path = url.upToFirstOccurrenceOf("?", false, false);

    if (path == "/" || path == "/index.html") { /* ... */ }
    if (path == "/js/juce/index.js") { /* ... */ }
    if (path == "/js/juce/check_native_interop.js") { /* ... */ }

    return std::nullopt;
}
```

## Verification

After this fix, the Standalone app rendered correctly on the very FIRST
launch after a rebuild, with no relaunch needed - a behavior change
observed consistently, whereas before the fix a relaunch was needed on
nearly every rebuild cycle during this session's testing.

## Prevention

**For this codebase generally:** any JUCE WebView plugin whose editor
calls `webView->goToURL(...)` with a **constant** URL (which is the
standard pattern - see `troubleshooting/patterns/juce8-critical-patterns.md`
Pattern #8/#21) is potentially susceptible to this stale-cache behavior,
especially during active development where the underlying HTML/JS/CSS
changes frequently between test launches. Consider applying the same
cache-busting query-string pattern proactively in the shared WebView editor
template (`ui-mockup` skill's `assets/webview-templates/PluginEditor-webview.cpp`),
not just as a reactive fix once someone hits confusing "my changes aren't
showing up" symptoms.

**Detection heuristic:** if a WebView plugin under active development shows
inconsistent behavior between launches of the SAME compiled binary - some
launches showing stale/old content, some blank, some correct - and you've
already confirmed (via `strings` on the binary) that the current build
genuinely contains your latest changes, suspect this caching behavior
before assuming a logic bug in the HTML/JS itself.

## Related Issues

- See also: `troubleshooting/build-failures/standalone-target-never-rebuilt-by-build-script-Transitionist-20260826.md` -
  a DIFFERENT stale-content bug (the build script never rebuilding the
  Standalone target at all) with a similar-sounding symptom (stale UI after
  a rebuild) but a completely different root cause. Both were present in
  this plugin's history at different points - always verify with `strings`
  on the actual compiled binary which failure mode you're looking at before
  choosing a fix.
