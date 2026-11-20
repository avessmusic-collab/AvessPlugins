; VOID by TÂCHES - Windows installer (Inno Setup 6)
; Compiled on CI by .github/workflows/void-windows.yml after building the
; Windows binaries (source folder is plugins/WARPR, product name VOID).

#define AppName "VOID"
#define AppVersion "1.0.0"
#define AppPublisher "TÂCHES"

[Setup]
AppId={{7A1E9C54-3B06-4F8D-A2C7-0000VOID1000}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={commoncf64}\VST3
AppendDefaultDirName=no
DisableDirPage=no
DirExistsWarning=no
DisableProgramGroupPage=yes
OutputBaseFilename=VOID-{#AppVersion}-Windows
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile=
InfoBeforeFile=ReadMeWin.txt

[Types]
Name: "full"; Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 Plugin (Ableton, FL Studio, Reaper, Cubase...)"; Types: full custom; Flags: fixed
Name: "standalone"; Description: "Standalone application"; Types: full

[Files]
; Build output paths (relative to this .iss) - the CI builds into build-win
; at the repository root. The VST3 goes into the user-chosen folder
; (defaults to the standard C:\Program Files\Common Files\VST3).
Source: "..\..\..\..\build-win\plugins\WARPR\WARPR_artefacts\Release\VST3\VOID.vst3\*"; \
    DestDir: "{app}\VOID.vst3"; Components: vst3; \
    Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\..\..\build-win\plugins\WARPR\WARPR_artefacts\Release\Standalone\VOID.exe"; \
    DestDir: "{autopf}\TACHES\VOID"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\VOID"; Filename: "{autopf}\TACHES\VOID\VOID.exe"; Components: standalone

[Run]
; WebView2 runtime is preinstalled on Win 11 / current Win 10; nothing to do.

[UninstallDelete]
; Force-remove the whole plugin bundle folders on uninstall. Without this,
; Inno removes the individually-tracked files but can leave the .vst3
; folder shell behind on disk.
Type: filesandordirs; Name: "{app}\VOID.vst3"
Type: filesandordirs; Name: "{autopf}\TACHES\VOID"
Type: dirifempty; Name: "{autopf}\TACHES"
