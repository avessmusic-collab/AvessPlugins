; CORRUPTR by Avess - Windows installer (Inno Setup 6)
; Compile with Inno Setup after building the Windows binaries
; (see BUILD-WINDOWS.md in this folder).

#define AppName "CORRUPTR"
#define AppVersion "1.0.0"
#define AppPublisher "Avess"

[Setup]
AppId={{8C4B2A31-7E52-4D2B-9C1F-CORRUPTR100}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={commoncf64}\VST3
AppendDefaultDirName=no
DisableDirPage=no
DirExistsWarning=no
DisableProgramGroupPage=yes
OutputBaseFilename=CORRUPTR-{#AppVersion}-Windows
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
; Build output paths - adjust if your build directory differs
; The VST3 goes into the user-chosen folder (defaults to the standard
; C:\Program Files\Common Files\VST3 - keep the default unless your DAW
; scans a custom VST3 path).
Source: "..\..\..\..\build-win\plugins\CORRUPTR\CORRUPTR_artefacts\Release\VST3\CORRUPTR.vst3\*"; \
    DestDir: "{app}\CORRUPTR.vst3"; Components: vst3; \
    Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\..\..\build-win\plugins\CORRUPTR\CORRUPTR_artefacts\Release\Standalone\CORRUPTR.exe"; \
    DestDir: "{autopf}\Avess\CORRUPTR"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\CORRUPTR"; Filename: "{autopf}\Avess\CORRUPTR\CORRUPTR.exe"; Components: standalone

[Run]
; WebView2 runtime is preinstalled on Win 11 / current Win 10; nothing to do.

[UninstallDelete]
; Force-remove the whole plugin bundle folders on uninstall. Without this,
; Inno removes the individually-tracked files but can leave the .vst3 /
; .component folder shells behind on disk.
Type: filesandordirs; Name: "{app}\CORRUPTR.vst3"
Type: filesandordirs; Name: "{autopf}\Avess\CORRUPTR"
Type: dirifempty; Name: "{autopf}\Avess"
; WebView2 per-user data folder created at runtime (see PluginEditor.cpp)
Type: filesandordirs; Name: "{localappdata}\Temp\CORRUPTR-WebView2"
