#ifndef AppVersion
#define AppVersion "0.3.1-credits.4"
#endif
#ifndef PayloadDir
#error PayloadDir is required
#endif
#ifndef OutputDir
#define OutputDir "..\dist"
#endif
[Setup]
AppId={{4932901D-91E6-4FE4-A79E-D63258D637F2}
AppName=NVOF for PotPlayer
AppVersion={#AppVersion}
AppPublisher=NVOF for PotPlayer contributors
AppPublisherURL=https://github.com/mkyoon80-alt/nvof-potplayer
AppSupportURL=https://github.com/mkyoon80-alt/nvof-potplayer/issues
DefaultDirName={localappdata}\Programs\NvofPotPlayer
DefaultGroupName=NVOF for PotPlayer
DisableProgramGroupPage=yes
DisableDirPage=no
DisableWelcomePage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19041
OutputDir={#OutputDir}
OutputBaseFilename=NvofPotPlayer-{#AppVersion}-Setup-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=115
WizardResizable=yes
WizardImageFile=wizard-image.bmp
WizardSmallImageFile=wizard-small.bmp
UninstallDisplayName=NVOF for PotPlayer
UninstallDisplayIcon={app}\NvofControl.exe
CloseApplications=no
RestartApplications=no
SetupLogging=yes
UsePreviousLanguage=yes
VersionInfoVersion=0.3.1.10
VersionInfoDescription=NVOF for PotPlayer offline setup
[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"
[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Excludes: "NvofPotPlayer.ini"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\NvofPotPlayer.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
[InstallDelete]
; Remove only known obsolete files from the application runtime subdirectory.
Type: files; Name: "{app}\runtime\NvOFFRUC.dll"
Type: files; Name: "{app}\runtime\NvofFrucBridge.dll"
Type: files; Name: "{app}\runtime\NVEncNVOFFRUC.dll"
Type: files; Name: "{app}\runtime\cudart64_110.dll"
Type: files; Name: "{app}\runtime\cudart64_12.dll"
Type: files; Name: "{app}\runtime\msvcp140.dll"
Type: files; Name: "{app}\runtime\vcruntime140.dll"
Type: files; Name: "{app}\runtime\vcruntime140_1.dll"
Type: files; Name: "{app}\licenses\NVIDIA-COMPONENT-TERMS.txt"
Type: files; Name: "{app}\licenses\NVIDIA-CUDA-11.2-EULA.html"
Type: files; Name: "{app}\licenses\NVEnc-MIT.txt"
[Icons]
Name: "{group}\NVOF 설정"; Filename: "{app}\NvofControl.exe"
Name: "{group}\사용 설명서"; Filename: "{app}\docs\manual\index.html"
Name: "{group}\NVOF 제거"; Filename: "{uninstallexe}"
Name: "{app}\NVOF 제거"; Filename: "{uninstallexe}"
Name: "{group}\설치 폴더 열기"; Filename: "{app}"
[Run]
Filename: "{app}\docs\manual\index.html"; Description: "사용 설명서 열기"; Flags: postinstall shellexec skipifsilent unchecked
Filename: "{app}\NvofControl.exe"; Description: "NVOF 설정 열기"; Flags: postinstall nowait skipifsilent
[Code]
const
  FilterKey = 'Software\Classes\CLSID\{EDECA044-78CD-40EB-8F37-63D947C501A0}\InprocServer32';
var
  PreviousFilter: String;
  RegisteredOK: Boolean;

function Korean: Boolean;
begin Result := ActiveLanguage = 'korean'; end;
function L(Ko, En: String): String;
begin if Korean then Result := Ko else Result := En; end;
function AppsClosed: Boolean;
var Services, Items: Variant;
begin
  Result := False;
  try
    Services := GetActiveOleObject('WbemScripting.SWbemLocator');
  except
    Services := CreateOleObject('WbemScripting.SWbemLocator');
  end;
  try
    Services := Services.ConnectServer('', 'root\CIMV2');
    Items := Services.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name="PotPlayerMini64.exe" OR Name="PotPlayer64.exe" OR Name="PotPlayerMini.exe" OR Name="NvofControl.exe"');
    Result := Items.Count = 0;
  except
    Log('Process check failed; refusing to replace in-use files.');
  end;
end;
function CloseMessage: String;
begin Result := L('팟플레이어와 NVOF 설정 창을 모두 닫은 뒤 다시 시도해 주세요. 프로그램을 강제로 종료하지 않습니다.', 'Close PotPlayer and NVOF Control, then retry. Setup will not force-close applications.'); end;
procedure InitializeWizard;
begin
  RegisteredOK := False;
  WizardForm.WelcomeLabel1.Caption := L('NVOF for PotPlayer 설치', 'Install NVOF for PotPlayer');
  WizardForm.WelcomeLabel2.Caption := L('팟플레이어용 NVIDIA 프레임 보간을 설치합니다.'#13#10#13#10'별도 .NET, CUDA Toolkit, Visual C++ 설치가 필요하지 않습니다.'#13#10#13#10'Windows x64, 지원 NVIDIA GPU·드라이버와 팟플레이어 x64는 필요합니다.'#13#10#13#10'설치 전에 팟플레이어와 NVOF 설정 창을 닫아 주세요.', 'Install NVIDIA frame interpolation for PotPlayer.'#13#10#13#10'.NET is included. No FRUC or CUDA runtime is bundled.'#13#10#13#10'Windows x64, a supported NVIDIA GPU/driver and PotPlayer x64 are required.'#13#10#13#10'Close PotPlayer and NVOF Control before installing.');
end;
function PrepareToInstall(var NeedsRestart: Boolean): String;
var Current: String;
begin
  Result := '';
  if not AppsClosed then begin Result := CloseMessage; exit; end;
  if RegQueryStringValue(HKLM64, FilterKey, '', Current) and (Current <> '') and (CompareText(Current, ExpandConstant('{app}\NvofPotPlayer.ax')) <> 0) then
    Log('Existing machine-wide registration is retained. This setup registers for the current user only.');
  RegQueryStringValue(HKCU64, FilterKey, '', PreviousFilter);
end;
procedure RestorePrevious;
var Code: Integer; Helper: String;
begin
  if (PreviousFilter = '') or (CompareText(PreviousFilter, ExpandConstant('{app}\NvofPotPlayer.ax')) = 0) then exit;
  Helper := ExtractFileDir(PreviousFilter) + '\NvofRegister.exe';
  if FileExists(PreviousFilter) and FileExists(Helper) then
    if not Exec(Helper, '--register', ExtractFileDir(Helper), SW_HIDE, ewWaitUntilTerminated, Code) then Log('Could not restore previous registration.');
end;
procedure CurStepChanged(CurStep: TSetupStep);
var Code: Integer; OldIni, NewIni, SavedPrevious: String;
begin
  if CurStep = ssInstall then begin
    ForceDirectories(ExpandConstant('{app}'));
    NewIni := ExpandConstant('{app}\NvofPotPlayer.ini');
    if FileExists(NewIni) then begin
      if not FileCopy(NewIni, NewIni + '.bak', False) then RaiseException('Cannot back up existing settings.');
    end else if PreviousFilter <> '' then begin
      OldIni := ExtractFileDir(PreviousFilter) + '\NvofPotPlayer.ini';
      if FileExists(OldIni) then if not FileCopy(OldIni, NewIni, True) then RaiseException('Cannot preserve previous settings.');
    end;
  end;
  if CurStep = ssPostInstall then begin
    SavedPrevious := GetIniString('Install', 'PreviousFilter', '', ExpandConstant('{app}\install-state.ini'));
    if (SavedPrevious = '') and (CompareText(PreviousFilter, ExpandConstant('{app}\NvofPotPlayer.ax')) <> 0) then
      SetIniString('Install', 'PreviousFilter', PreviousFilter, ExpandConstant('{app}\install-state.ini'));
    { The 0.3 playback backend is lab11. Preserve user switches/rate selection. }
    if not SetIniString('Nvof', 'ExperimentalNativeSynthesis', '1', ExpandConstant('{app}\NvofPotPlayer.ini')) then
      RaiseException('Cannot select the 0.3 interpolation backend.');
    if not Exec(ExpandConstant('{app}\NvofRegister.exe'), '--register', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
    if Code = 0 then
      if not Exec(ExpandConstant('{app}\NvofRegister.exe'), '--verify', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
    if Code <> 0 then begin
      Exec(ExpandConstant('{app}\NvofRegister.exe'), '--unregister', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code);
      RestorePrevious;
      RaiseException(L('필터 등록에 실패했습니다. 기존 등록 복원을 시도했습니다. 설치 로그를 확인해 주세요.', 'Filter registration failed. Previous registration restoration was attempted. Check the setup log.'));
    end;
    RegisteredOK := True;
    WizardForm.FinishedLabel.Caption := L('설치와 필터 등록이 완료되었습니다.'#13#10#13#10'팟플레이어 F5 → 코덱/필터 → 전역 필터 우선 순위에서 NVIDIA Optical Flow for PotPlayer를 추가하고 최우선 사용으로 설정해 주세요.'#13#10#13#10'자세한 순서는 사용 설명서에 있습니다.', 'Installation and filter registration completed.'#13#10#13#10'Add NVIDIA Optical Flow for PotPlayer in PotPlayer Preferences → Filter Control → Global Filter Priority and select Prefer.'#13#10#13#10'See the user guide for the complete steps.');
  end;
end;
function GetCustomSetupExitCode: Integer;
begin if RegisteredOK then Result := 0 else Result := 10; end;
function InitializeUninstall: Boolean;
var Attempt: Integer;
begin
  Result := False;
  { The settings window closes immediately after launching this uninstaller. }
  for Attempt := 1 to 10 do begin
    if AppsClosed then begin Result := True; exit; end;
    Sleep(200);
  end;
  SuppressibleMsgBox(CloseMessage, mbError, MB_OK, IDOK);
end;
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Current: String; Code: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    { Do this after the user's uninstall confirmation, but before any deletion.
      Elevate the helper only, keeping HKCU bound to the original user even
      when UAC is approved with a different administrator account. }
    if RegQueryStringValue(HKLM64, FilterKey, '', Current) and (CompareText(Current, ExpandConstant('{app}\NvofPotPlayer.ax')) = 0) then begin
      if not ShellExec('runas', ExpandConstant('{app}\NvofRegister.exe'), '--unregister-machine', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
      if Code <> 0 then RaiseException(L('관리자용 필터 등록을 해제하지 못해 제거를 중단했습니다. 파일은 유지됩니다. 다시 제거하고 Windows 권한 요청을 승인해 주세요.', 'Machine filter unregistration failed or was cancelled. Files were retained. Retry uninstall and approve the Windows permission request.'));
      if RegQueryStringValue(HKLM64, FilterKey, '', Current) and (CompareText(Current, ExpandConstant('{app}\NvofPotPlayer.ax')) = 0) then
        RaiseException('Machine filter registration remains. Removal stopped.');
    end;
    if RegQueryStringValue(HKCU64, FilterKey, '', Current) and (CompareText(Current, ExpandConstant('{app}\NvofPotPlayer.ax')) = 0) then begin
      if not Exec(ExpandConstant('{app}\NvofRegister.exe'), '--unregister', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
      if Code <> 0 then RaiseException('Filter unregistration failed. Removal stopped.');
      if RegQueryStringValue(HKCU64, FilterKey, '', Current) and (CompareText(Current, ExpandConstant('{app}\NvofPotPlayer.ax')) = 0) then
        RaiseException('User filter registration remains. Removal stopped.');
    end;
    { Normal removal must not silently reactivate an older filter. Installation
      failure still uses RestorePrevious for rollback. Other folders are kept. }
  end;
end;
