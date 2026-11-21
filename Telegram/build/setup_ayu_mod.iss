#define MyAppName "AyuGramMod"
#define MyAppShortName "AyuGramMod"
#define MyAppExeName "AyuGramMod.exe"
#define MyAppPublisher "AyuGramMod"
#define MyAppId "8D9A56B2-6C1E-4FD8-9B77-1B0B9E3D3A21"

#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#ifndef ReleasePath
  #define ReleasePath "..\..\out\deploy"
#endif
#ifndef OutputDir
  #define OutputDir "..\.."
#endif
#ifndef OutputBaseFilename
  #define OutputBaseFilename "AyuGramMod-Setup-x64"
#endif
#ifndef ChineseSimplifiedLanguage
  #define ChineseSimplifiedLanguage "compiler:Languages\ChineseSimplified.isl"
#endif

[Languages]
Name: "chs"; MessagesFile: "{#ChineseSimplifiedLanguage}"

[Setup]
AppId={{{#MyAppId}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=no
AllowNoIcons=yes
LanguageDetectionMethod=uilanguage
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion=1.0.0.0
WizardStyle=modern

[Files]
Source: "{#ReleasePath}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleasePath}\Updater.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleasePath}\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#ReleasePath}\modules\*"; DestDir: "{app}\modules"; Flags: recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppShortName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppShortName}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppShortName}"; Filename: "{app}\{#MyAppExeName}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppShortName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\modules"
Type: filesandordirs; Name: "{app}\tupdates"
Type: filesandordirs; Name: "{app}\tdata"
Type: filesandordirs; Name: "{app}\tcache"
Type: filesandordirs; Name: "{app}\tdumps"
Type: dirifempty; Name: "{app}"
