Transitionist by TÂCHES - Installation Guide
Version 1.0.0

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

WHAT'S INCLUDED

This distribution package contains:
- Transitionist-by-TACHES.pkg (3.3 MB) - Branded installer with setup wizard
- install-readme.txt (this file) - Installation instructions

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

INSTALLATION STEPS

1. Copy Transitionist-by-TACHES.pkg to the other Mac (AirDrop, USB drive,
   Dropbox/Google Drive/iCloud, email - any file transfer method works)
2. Double-click "Transitionist-by-TACHES.pkg" to launch the installer
3. Read the Welcome screen and click Continue
4. Read the ReadMe and click Continue
5. Click Install (may require administrator password)
6. Wait for installation to complete
7. Read the Conclusion screen with quick start tips

Transitionist will be installed to:
- ~/Library/Audio/Plug-Ins/VST3/Transitionist.vst3
- ~/Library/Audio/Plug-Ins/Components/Transitionist.component

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

FIRST USE - IMPORTANT (macOS Security)

macOS will show a security warning on first use because this plugin is
not notarized by Apple. This is normal for independent plugins.

To authorize Transitionist:

1. Open your DAW (Logic Pro, Ableton Live, FL Studio, etc.)
2. Try to load Transitionist on an audio track
3. macOS will block it with a security warning dialog
4. Open System Settings > Privacy & Security
5. Scroll down to the Security section
6. Click "Open Anyway" next to the Transitionist warning
7. Confirm you want to open it
8. Return to your DAW and try loading Transitionist again
9. Click "Open" when prompted

This is a ONE-TIME process for each plugin format (VST3 and AU).
After authorization, Transitionist will load normally every time.

The installer also strips the macOS quarantine flag from both bundles
during install, which avoids the "app is damaged and can't be opened"
false-positive some macOS versions show for unsigned plugin bundles. If
you still see that specific message (rather than the normal "unidentified
developer" warning), run this in Terminal as a fallback:

  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Transitionist.vst3
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/Transitionist.component

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

ABOUT TRANSITIONIST

Transitionist throws the incoming audio into an escalating delay+reverb
wash that builds tension, then lets it be filter-swept away - the classic
riser/breakdown/DJ-transition move, distilled into three macro knobs.

Signal chain: Delay -> Reverb -> Bipolar DJ Filter -> Output Glue -> Width

PARAMETERS:
- THROW: 0-100%, default 0% - combined dry/wet, delay feedback amount, and
  reverb decay/size. Near the top of its range, engages reverb freeze/hold
  for an infinite, automatable tail (the "throw and hold" gesture).
- SPACE: 0-100%, default 0% - links delay time (tempo-synced division) and
  reverb size into one "room" macro - tight slapback to cavernous wash.
- SWEEP: -100% to +100% (bipolar), default 0% (center/fully open) -
  bipolar DJ-style filter. Center is open. CCW closes a 24dB lowpass, CW
  opens a 24dB highpass, resonance rises toward both extremes.

FEATURES:
- Light skeuomorphic hardware UI - brushed-metal knobs, LCD-style readouts,
  gray textured chassis
- Quick preset bar (prev/next + dropdown) with 8 starter presets: Init,
  Gentle Wash, Tight Slap, Big Riser, Infinite Freeze, Filter Down Sweep,
  Filter Up Open, Cavernous Hold
- SWEEP has a center-detent "snap" feel while dragging, and a signed LCD
  readout (-NN% / +NN% / 0%)
- Full DAW automation support (touch/latch recording works correctly)

QUICK START:
Try the preset bar first (top of the plugin window) to hear the range of
the effect, then fine-tune with the three knobs. Automate SWEEP for a
classic filter-sweep transition, or automate THROW toward 100% for a
"throw and hold" riser into a drop.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

FINDING TRANSITIONIST IN YOUR DAW

VST3: Look in your plugin browser under "TÂCHES" or "Transitionist"
AU: Audio Units > Effect > TÂCHES > Transitionist

Some DAWs require a plugin rescan after installation.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

TROUBLESHOOTING

Plugin doesn't appear in DAW:
-> Rescan plugins in your DAW preferences
-> Verify files exist in ~/Library/Audio/Plug-Ins/

Security warning won't go away:
-> Make sure you clicked "Open Anyway" in System Settings
-> Try restarting your DAW after authorization
-> Complete authorization for both VST3 and AU formats

Plugin crashes on load:
-> Check macOS version (14.6+ recommended)
-> Make sure DAW is up to date
-> Try deleting and reinstalling

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

UNINSTALLATION

To remove Transitionist:

1. Quit your DAW
2. Delete these files:
   - ~/Library/Audio/Plug-Ins/VST3/Transitionist.vst3
   - ~/Library/Audio/Plug-Ins/Components/Transitionist.component
3. Rescan plugins in your DAW

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

SYSTEM REQUIREMENTS

- macOS 14.6 or later recommended
- Compatible DAW (Logic Pro, Ableton Live, FL Studio, Reaper, etc.)
- Intel or Apple Silicon Mac (built for this machine's architecture -
  if the other Mac has a different chip type, e.g. this one is Apple
  Silicon and the other is Intel, a Universal Binary rebuild would be
  needed; ask if that's the case)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

SUPPORT

For questions or issues, contact TÂCHES.

Enjoy making transitions with Transitionist!

— TÂCHES
