#ifndef Target
  #error Set Target to Win7 or Win10-11
#endif
#define Product "Toggle External Connection"
#define Binary "ToggleExternalConnection.exe"
[Setup]
AppId={{CA78C24A-7964-4F3A-8F40-DA64E9D82A11}
AppName={#Product}
AppVersion=0.1.0-rc.1
AppPublisher=yunhyok
AppPublisherURL=https://github.com/yunhyok/toggle-external-connection
DefaultDirName={commonpf64}\{#Product}
DisableDirPage=yes
UsePreviousAppDir=no
DefaultGroupName={#Product}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#if Target == "Win7"
MinVersion=6.1sp1
OnlyBelowVersion=6.2
#elif Target == "Win10-11"
MinVersion=10.0
#else
  #error Unknown target
#endif
AppMutex=Global\ToggleExternalConnection.Production
OutputDir=..\dist
OutputBaseFilename=ToggleExternalConnection-{#Target}-x64-Setup
SetupIconFile=..\assets\connected.ico
UninstallDisplayIcon={app}\{#Binary}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"
[Files]
Source: "..\build\{#Target}\{#Binary}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\TESTING.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\scripts\check-lab.ps1"; DestDir: "{app}"; Flags: ignoreversion
[Icons]
Name: "{group}\{#Product}"; Filename: "{app}\{#Binary}"
[UninstallDelete]
Type: files; Name: "{app}\Data\history-????????.log"
Type: files; Name: "{app}\Data\history-????????.log.tmp"
Type: files; Name: "{app}\Data\settings.dat"
Type: files; Name: "{app}\Data\settings.tmp"
Type: dirifempty; Name: "{app}\Data"
[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if CompareText(ExpandFileName(WizardDirValue), ExpandFileName(ExpandConstant('{commonpf64}\{#Product}'))) <> 0 then
    Result := 'Install this application in its protected Program Files directory.'
  else Result := '';
end;
