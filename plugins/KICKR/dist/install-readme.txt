KICKR v1.0.0 - Installation Instructions
===========================================

INSTALLATION:
1. Double-click "KICKR.pkg"
2. On the "Installation Type" screen, choose which format(s) to install —
   VST3, Audio Unit (AU), or both (both are checked by default). Click
   "Customize" if that screen doesn't appear automatically.
3. Follow the remaining installation prompts
4. The installer copies your selected format(s) to:
   - VST3: ~/Library/Audio/Plug-Ins/VST3/KICKR.vst3
   - AU: ~/Library/Audio/Plug-Ins/Components/KICKR.component

FIRST USE (IMPORTANT):
macOS will show a security warning because this plugin is not notarized
(it is ad-hoc code signed by the installer, which avoids a "damaged" error
but still isn't an Apple Developer ID signature).

To bypass Gatekeeper:
1. Open your DAW (Logic Pro, Ableton, etc.)
2. When you first load KICKR, macOS will block it
3. Go to System Settings > Privacy & Security
4. Scroll down and click "Open Anyway" next to the KICKR warning
5. Confirm you want to open it

You only need to do this once per plugin format you installed.

PLUGIN INFO:
- Version: 1.0.0
- Formats: VST3, AU (choose one or both during install)
- Parameters: 60 (Pitch, Body incl. the MORPH knob, Sub, Click, Tail, Noise,
  Sample, Distortion, Tone, Stereo, Output, Tuning, 4 Macros)
- MORPH (big knob, left of the scope): skews the body waveform toward a soft
  saw ON THE ATTACK only — the tail always relaxes back to a clean sine.
  0 = pure sine (the classic KICKR sound).
- Strictly monophonic: a new note always cuts the previous one instantly
  (0.75 ms declick), no overlapping hits, no stuck tails.
- Includes: 50 built-in factory kick samples, 17 factory presets
- Description: a commercial-grade kick-drum design instrument - algorithmic
  synthesis blendable with a sample layer, 7-curve morphing distortion,
  real-time waveform/spectrum analyzer (sample-accurate onset - the scope
  always starts at the note-on, at any retrigger speed), Randomize/Mutate/
  Undo. Hold Shift while dragging any knob for a smooth 5x finer micro-
  adjust. Sub Frequency reads out as a note + Hz, e.g. "D#1 (40.0 Hz)".

UNINSTALLATION:
To remove KICKR:
1. Delete ~/Library/Audio/Plug-Ins/VST3/KICKR.vst3
2. Delete ~/Library/Audio/Plug-Ins/Components/KICKR.component
3. Restart your DAW

Your own recorded kicks (if you added any) live separately in
~/Music/KICKR/Samples/ and ~/Music/KICKR/Presets/ - delete those too if
you want a complete removal.

COMPATIBILITY:
- macOS 14.6 or later recommended
- Built for your Mac architecture
