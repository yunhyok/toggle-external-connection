#ifndef Target
  #error Set Target to Win7 or Win10-11
#endif
#define Product "Toggle External Connection"
#define Binary "ToggleExternalConnection.exe"
[Setup]
AppId={{CA78C24A-7964-4F3A-8F40-DA64E9D82A11}
AppName={#Product}
AppVersion=0.3.0
AppPublisher=yunhyok
AppPublisherURL=https://github.com/yunhyok/toggle-external-connection
DefaultDirName={commonpf64}\{#Product}
DisableDirPage=no
UsePreviousAppDir=yes
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
Source: "..\build\{#Target}\install-guard.exe"; Flags: dontcopy
Source: "..\build\{#Target}\{#Binary}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\TESTING.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\scripts\check-lab.ps1"; DestDir: "{app}"; Flags: ignoreversion
[Icons]
Name: "{group}\{#Product}"; Filename: "{app}\{#Binary}"
Name: "{group}\GitHub Web"; Filename: "{app}\{#Binary}"; Parameters: "--github-web"
[UninstallDelete]
Type: files; Name: "{app}\Data\history-????????.log"
Type: files; Name: "{app}\Data\history-????????.log.tmp"
Type: files; Name: "{app}\Data\settings.dat"
Type: files; Name: "{app}\Data\settings.tmp"
Type: dirifempty; Name: "{app}\Data"
[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{CA78C24A-7964-4F3A-8F40-DA64E9D82A11}_is1';
var
  GuardExtracted: Boolean;
  PreserveGoogleLogin: Boolean;
  DestinationNote: TNewStaticText;

function RunGuard(const Mode: String): Boolean;
var
  ExitCode: Integer;
begin
  if not GuardExtracted then begin
    ExtractTemporaryFile('install-guard.exe');
    GuardExtracted := True;
  end;
  Result := Exec(ExpandConstant('{tmp}\install-guard.exe'), Mode + ' "' + WizardDirValue + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode) and (ExitCode = 0);
end;

procedure InitializeWizard;
begin
  DestinationNote := TNewStaticText.Create(WizardForm);
  DestinationNote.Parent := WizardForm.SelectDirPage;
  DestinationNote.Left := WizardForm.DirEdit.Left;
  DestinationNote.Top := WizardForm.DirEdit.Top + WizardForm.DirEdit.Height + ScaleY(16);
  DestinationNote.Width := WizardForm.SelectDirBrowseLabel.Width;
  DestinationNote.AutoSize := False;
  DestinationNote.WordWrap := True;
  DestinationNote.Caption := CustomMessage('DestinationHelp');
  DestinationNote.AdjustHeight;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  PreviousDir, Uninstaller: String;
  GoogleLogin: Cardinal;
  ExitCode: Integer;
begin
  Result := '';
  if RegQueryDWordValue(HKLM64, UninstallKey, 'TECAllowGoogleLogin', GoogleLogin) then
    PreserveGoogleLogin := GoogleLogin = 1;
  if not RunGuard('--prepare') then begin
    Result := CustomMessage('UnsafeDestination');
    Exit;
  end;
  if RegQueryStringValue(HKLM64, UninstallKey, 'InstallLocation', PreviousDir) and
    (CompareText(RemoveBackslash(ExpandFileName(PreviousDir)), RemoveBackslash(ExpandFileName(WizardDirValue))) <> 0) then begin
    if not RegQueryStringValue(HKLM64, UninstallKey, 'UninstallString', Uninstaller) then begin
      Result := CustomMessage('MigrationFailed');
      Exit;
    end;
    Uninstaller := RemoveQuotes(Uninstaller);
    if (CompareText(RemoveBackslash(ExtractFileDir(Uninstaller)), RemoveBackslash(PreviousDir)) <> 0) or
      (CompareText(ExtractFileName(Uninstaller), 'unins000.exe') <> 0) or
      not RunGuard('--migrate') then begin
      Result := CustomMessage('MigrationFailed');
      Exit;
    end;
    if not Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE,
      ewWaitUntilTerminated, ExitCode) or (ExitCode <> 0) or FileExists(PreviousDir + '\{#Binary}') then
      Result := CustomMessage('MigrationFailed');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and not RunGuard('--seal') then
    RaiseException(CustomMessage('UnsafeDestination'));
  if (CurStep = ssPostInstall) and PreserveGoogleLogin then
    if not RegWriteDWordValue(HKLM64, UninstallKey, 'TECAllowGoogleLogin', 1) then
      RaiseException('Could not restore the Google sign-in preference.');
end;

[CustomMessages]
english.DestinationHelp=Choose the folder and drive. Use a local NTFS drive and parent folders protected from changes by ordinary users. When moving an existing installation, Setup verifies a copy of Data in the new folder before removing the old installation. Settings and history are kept.
korean.DestinationHelp=설치할 드라이브와 폴더를 선택하세요. 로컬 NTFS 드라이브와 일반 사용자가 변경할 수 없는 상위 폴더가 필요합니다. 기존 설치 위치를 바꾸면 Data를 새 폴더에 복사·검증한 뒤 이전 설치를 제거합니다. 설정·기록은 유지됩니다.
english.UnsafeDestination=This folder is not safe for an application that runs as administrator. Choose a new program folder directly under a local NTFS drive or inside a protected folder. Network/removable drives, links, and folders that ordinary users can replace are not supported.
korean.UnsafeDestination=관리자 권한으로 실행하는 앱을 안전하게 설치할 수 없는 경로입니다. 로컬 NTFS 드라이브 바로 아래의 새 프로그램 폴더 또는 보호된 상위 폴더 안을 선택하세요. 네트워크·이동식 드라이브, 링크, 일반 사용자가 바꿀 수 있는 폴더는 지원하지 않습니다.
english.MigrationFailed=Setup could not safely move the existing installation. Its data is kept in the old folder, or a verified copy is in the selected folder if removal already started. Do not delete either Data folder. Check disk space and folder permissions, close the app, then retry. Choose a destination outside the old installation tree.
korean.MigrationFailed=기존 설치를 안전하게 옮기지 못했습니다. 데이터는 이전 폴더에 보존되며, 제거가 시작된 경우 선택한 폴더에 검증된 복사본이 있습니다. 두 Data 폴더를 삭제하지 마세요. 디스크 공간·폴더 권한을 확인하고 앱을 닫은 뒤 다시 시도하세요. 이전 설치 폴더 밖의 경로를 선택하세요.
