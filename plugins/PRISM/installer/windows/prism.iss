; PRISM by TÂCHES — Windows installer (Inno Setup 6)
; Installs the VST3 bundle to the standard system VST3 folder.

#define AppName "PRISM"
#define AppVersion "1.0.0"
#define Publisher "TÂCHES"

[Setup]
AppId={{7B1C9A2E-5D14-4F6B-9E2A-PRISM1000TCH}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#Publisher}
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputBaseFilename=PRISM-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
SetupIconFile=
UninstallDisplayName={#AppName} {#AppVersion} by {#Publisher}

[Messages]
WelcomeLabel1=Welcome to the [name] installer
WelcomeLabel2=This will install [name/ver] by TÂCHES on your computer.%n%nPRISM is a real-time audio analyzer and visualizer: calibrated spectrum, EBU R128 loudness, true peak, stereo imaging, and a DJ-grade beat-synced waveform with an editable beatgrid.%n%nClose your DAW before continuing.

[Files]
; The VST3 "bundle" is a directory on Windows too (VST3 3.6.10+ bundle format).
Source: "{#SourcePath}\..\..\..\..\build\plugins\PRISM\PRISM_artefacts\Release\VST3\PRISM.vst3\*"; DestDir: "{commoncf64}\VST3\PRISM.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\PRISM.vst3"

[Code]
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption :=
      'PRISM has been installed to the system VST3 folder.' + #13#10 + #13#10 +
      'Open your DAW (Ableton Live, Reaper, FL Studio, Cubase...), rescan plugins ' +
      'if needed, and load PRISM on any audio track.' + #13#10 + #13#10 +
      'Quick start: Settings (gear) for 7 color themes and presets; GRID chip for ' +
      'beatgrid editing; F for fullscreen; mouse wheel over the waveform zooms.' + #13#10 + #13#10 +
      'Requires the Microsoft Edge WebView2 Runtime (preinstalled on Windows 11 ' +
      'and most Windows 10 systems).';
end;
