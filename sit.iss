; Inno Setup script; CI runs it on the folder `make`-d and packaged as sit/
; (sit.exe, app/*.dll, vendor/). The DLLs go next to sit.exe so that plain
; shortcuts work without sit.cmd.
#ifndef Version
#define Version "0.1"
#endif

[Setup]
AppName=sit
AppVersion={#Version}
AppPublisher=sit
DefaultDirName={localappdata}\Programs\sit
DefaultGroupName=sit
PrivilegesRequired=lowest
OutputBaseFilename=sit-setup
OutputDir=.
SetupIconFile=sit.ico
UninstallDisplayIcon={app}\sit.exe
Compression=lzma2
SolidCompression=yes
DisableProgramGroupPage=yes

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "sit\sit.exe"; DestDir: "{app}"
Source: "sit\app\*"; DestDir: "{app}"; Flags: recursesubdirs
Source: "sit\vendor\*"; DestDir: "{app}\vendor"; Flags: recursesubdirs

[Icons]
Name: "{autoprograms}\sit"; Filename: "{app}\sit.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\sit"; Filename: "{app}\sit.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\sit.exe"; Description: "Start sit"; Flags: nowait postinstall skipifsilent
