#ifndef AppVersion
#define AppVersion "0.2.0-beta.1"
#endif
#ifndef PayloadDir
#error PayloadDir is required
#endif
#ifndef OutputDir
#define OutputDir "..\dist"
#endif
#define TermsVersion "2026-10-07"
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
VersionInfoVersion=0.2.0.9
VersionInfoDescription=NVOF for PotPlayer offline setup
[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"
[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Excludes: "NvofPotPlayer.ini"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\NvofPotPlayer.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#PayloadDir}\licenses\NVIDIA-COMPONENT-TERMS.txt"; Flags: dontcopy
[Icons]
Name: "{group}\NVOF 설정"; Filename: "{app}\NvofControl.exe"
Name: "{group}\사용 설명서"; Filename: "{app}\docs\manual\index.html"
Name: "{group}\NVOF 제거"; Filename: "{uninstallexe}"
[Run]
Filename: "{app}\docs\manual\index.html"; Description: "사용 설명서 열기"; Flags: postinstall shellexec skipifsilent unchecked
Filename: "{app}\NvofControl.exe"; Description: "NVOF 설정 열기"; Flags: postinstall nowait skipifsilent
[Code]
const
  FilterKey = 'Software\Classes\CLSID\{EDECA044-78CD-40EB-8F37-63D947C501A0}\InprocServer32';
var
  TermsPage: TWizardPage;
  TermsMemo: TNewMemo;
  TermsCheck: TNewCheckBox;
  TermsButton: TNewButton;
  PreviousFilter: String;
  RegisteredOK: Boolean;

function Korean: Boolean;
begin Result := ActiveLanguage = 'korean'; end;
function L(Ko, En: String): String;
begin if Korean then Result := Ko else Result := En; end;
function HasConsent: Boolean;
begin
  Result := TermsCheck.Checked;
  if WizardSilent then Result := ExpandConstant('{param:NVIDIATERMS|}') = '{#TermsVersion}';
end;
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
procedure TermsChanged(Sender: TObject);
begin WizardForm.NextButton.Enabled := HasConsent; end;
procedure OpenTerms(Sender: TObject);
var Code: Integer;
begin
  ShellExec('open', ExpandConstant('{tmp}\NVIDIA-COMPONENT-TERMS.txt'), '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;
procedure InitializeWizard;
begin
  RegisteredOK := False;
  WizardForm.WelcomeLabel1.Caption := L('NVOF for PotPlayer 설치', 'Install NVOF for PotPlayer');
  WizardForm.WelcomeLabel2.Caption := L('팟플레이어용 NVIDIA 프레임 보간을 설치합니다.'#13#10#13#10'별도 .NET, CUDA Toolkit, Visual C++ 설치가 필요하지 않습니다.'#13#10#13#10'Windows x64, 지원 NVIDIA GPU·드라이버와 팟플레이어 x64는 필요합니다.'#13#10#13#10'설치 전에 팟플레이어와 NVOF 설정 창을 닫아 주세요.', 'Install NVIDIA frame interpolation for PotPlayer.'#13#10#13#10'.NET and required CUDA / Visual C++ runtime files are included.'#13#10#13#10'Windows x64, a supported NVIDIA GPU/driver and PotPlayer x64 are required.'#13#10#13#10'Close PotPlayer and NVOF Control before installing.');
  TermsPage := CreateCustomPage(wpWelcome, L('NVIDIA 구성요소 이용 약관', 'NVIDIA Component Terms'), L('프로젝트 MIT 라이선스와 구분되는 외부 구성요소 약관입니다.', 'Separate terms apply to NVIDIA components, outside the project MIT license.'));
  ExtractTemporaryFile('NVIDIA-COMPONENT-TERMS.txt');
  TermsMemo := TNewMemo.Create(TermsPage);
  TermsMemo.Parent := TermsPage.Surface;
  TermsMemo.SetBounds(0, 0, TermsPage.SurfaceWidth, TermsPage.SurfaceHeight - ScaleY(78));
  TermsMemo.Anchors := [akLeft, akTop, akRight, akBottom];
  TermsMemo.ReadOnly := True;
  TermsMemo.ScrollBars := ssVertical;
  TermsMemo.WordWrap := True;
  TermsMemo.Lines.LoadFromFile(ExpandConstant('{tmp}\NVIDIA-COMPONENT-TERMS.txt'));
  TermsButton := TNewButton.Create(TermsPage);
  TermsButton.Parent := TermsPage.Surface;
  TermsButton.SetBounds(0, TermsPage.SurfaceHeight - ScaleY(69), ScaleX(150), ScaleY(26));
  TermsButton.Anchors := [akLeft, akBottom];
  TermsButton.Caption := L('약관 원문 열기 / 저장', 'Open / save terms');
  TermsButton.OnClick := @OpenTerms;
  TermsCheck := TNewCheckBox.Create(TermsPage);
  TermsCheck.Parent := TermsPage.Surface;
  TermsCheck.SetBounds(0, TermsPage.SurfaceHeight - ScaleY(32), TermsPage.SurfaceWidth, ScaleY(32));
  TermsCheck.Anchors := [akLeft, akRight, akBottom];
  TermsCheck.Caption := L('NVIDIA 구성요소 이용 약관에 동의합니다.', 'I agree to the NVIDIA component terms.');
  TermsCheck.Checked := False;
  TermsCheck.OnClick := @TermsChanged;
end;
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = TermsPage.ID then WizardForm.NextButton.Enabled := HasConsent;
end;
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = TermsPage.ID then begin
    Result := HasConsent;
    if not Result then Log('Explicit NVIDIA component consent was not given.');
  end;
end;
function PrepareToInstall(var NeedsRestart: Boolean): String;
var Current: String;
begin
  Result := '';
  if not HasConsent then begin
    Result := L('약관에 동의해야 설치할 수 있습니다.', 'Explicit acceptance of the component terms is required.'); exit;
  end;
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
    if not Exec(ExpandConstant('{app}\NvofRegister.exe'), '--register', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
    if Code = 0 then
      if not Exec(ExpandConstant('{app}\NvofRegister.exe'), '--verify', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
    if Code <> 0 then begin
      Exec(ExpandConstant('{app}\NvofRegister.exe'), '--unregister', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code);
      RestorePrevious;
      RaiseException(L('필터 등록에 실패했습니다. 기존 등록 복원을 시도했습니다. 설치 로그를 확인해 주세요.', 'Filter registration failed. Previous registration restoration was attempted. Check the setup log.'));
    end;
    if not SetIniString('Consent', 'TermsVersion', '{#TermsVersion}', ExpandConstant('{app}\component-consent.ini')) or
       not SetIniString('Consent', 'AppVersion', '{#AppVersion}', ExpandConstant('{app}\component-consent.ini')) or
       not SetIniString('Consent', 'AcceptedAtLocal', GetDateTimeString('yyyy-mm-dd hh:nn:ss', '-', ':'), ExpandConstant('{app}\component-consent.ini')) or
       not SetIniString('Consent', 'Method', 'explicit-interactive-or-versioned-command-line', ExpandConstant('{app}\component-consent.ini')) then begin
      Exec(ExpandConstant('{app}\NvofRegister.exe'), '--unregister', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code);
      RestorePrevious;
      RaiseException('Cannot save component consent record. Previous registration restoration was attempted.');
    end;
    RegisteredOK := True;
    WizardForm.FinishedLabel.Caption := L('설치와 필터 등록이 완료되었습니다.'#13#10#13#10'팟플레이어 F5 → 코덱/필터 → 전역 필터 우선 순위에서 NVIDIA Optical Flow for PotPlayer를 추가하고 최우선 사용으로 설정해 주세요.'#13#10#13#10'자세한 순서는 사용 설명서에 있습니다.', 'Installation and filter registration completed.'#13#10#13#10'Add NVIDIA Optical Flow for PotPlayer in PotPlayer Preferences → Filter Control → Global Filter Priority and select Prefer.'#13#10#13#10'See the user guide for the complete steps.');
  end;
end;
function GetCustomSetupExitCode: Integer;
begin if RegisteredOK then Result := 0 else Result := 10; end;
function InitializeUninstall: Boolean;
var Current: String;
begin
  Result := False;
  if not AppsClosed then begin SuppressibleMsgBox(CloseMessage, mbError, MB_OK, IDOK); exit; end;
  if RegQueryStringValue(HKLM64, FilterKey, '', Current) and (CompareText(Current, ExpandConstant('{app}\NvofPotPlayer.ax')) = 0) then begin
    SuppressibleMsgBox(L('이 폴더의 관리자용 필터가 등록되어 있습니다. 관리자 PowerShell에서 이 설치 폴더의 NvofRegister.exe --unregister-machine을 실행한 뒤 다시 제거해 주세요. 사용 설명서에 안내가 있습니다.', 'Run this folder''s NvofRegister.exe --unregister-machine from an elevated PowerShell, then uninstall. See the user guide.'), mbError, MB_OK, IDOK); exit;
  end;
  Result := True;
end;
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Current: String; Code: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    if RegQueryStringValue(HKCU64, FilterKey, '', Current) and (CompareText(Current, ExpandConstant('{app}\NvofPotPlayer.ax')) = 0) then begin
      if not Exec(ExpandConstant('{app}\NvofRegister.exe'), '--unregister', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
      if Code <> 0 then RaiseException('Filter unregistration failed. Removal stopped.');
      PreviousFilter := GetIniString('Install', 'PreviousFilter', '', ExpandConstant('{app}\install-state.ini'));
      RestorePrevious;
    end;
  end;
end;
