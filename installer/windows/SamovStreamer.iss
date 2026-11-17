; Inno Setup script – builds SamovStreamer-Setup-Windows.exe
; Called from CI:  ISCC.exe /DAppVersion=1.1.0 /DSourceDir=<path to "Samov Streamer.vst3"> SamovStreamer.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\plugin\build\MixStreamer_artefacts\Release\VST3\Samov Streamer.vst3"
#endif

[Setup]
AppId={{B5E3C0A2-6D1F-4C3B-9E7A-2F4D8A1C5E90}
AppName=Samov Streamer
AppVersion={#AppVersion}
AppVerName=Samov Streamer {#AppVersion}
AppPublisher=Samo Pospíšil
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
UninstallFilesDir={autopf}\Samov Streamer
UninstallDisplayName=Samov Streamer (VST3)
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir=output
OutputBaseFilename=SamovStreamer-Setup-Windows

[Languages]
Name: "cz"; MessagesFile: "compiler:Languages\Czech.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[InstallDelete]
; old name + previous version of this one
Type: filesandordirs; Name: "{commoncf64}\VST3\Mix Streamer.vst3"
Type: filesandordirs; Name: "{commoncf64}\VST3\Samov Streamer.vst3"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{commoncf64}\VST3\Samov Streamer.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Samov Streamer.vst3"

[Messages]
cz.FinishedLabel=Plugin Samov Streamer je nainštalovaný.%n%nReštartuj Cubase (alebo inú DAW) a vlož plugin na master.
en.FinishedLabel=Samov Streamer has been installed.%n%nRestart your DAW and insert the plugin on the master bus.
