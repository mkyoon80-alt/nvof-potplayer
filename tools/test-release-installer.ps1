param([Parameter(Mandatory=$true)][string]$Setup, [string]$TestRoot, [string]$NativeDirectory)
$ErrorActionPreference='Stop'
$root=if($TestRoot){[IO.Path]::GetFullPath($TestRoot)}else{[IO.Path]::GetFullPath((Join-Path $PSScriptRoot ('../build/installer-qa-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))))}
New-Item -ItemType Directory $root -Force | Out-Null
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $NativeDirectory){$NativeDirectory=Join-Path $project 'build/release030/native/Release'}
$destination=Join-Path $root '설치 검증 2'
$declined=Join-Path $root '미동의 검증'
$key='Software\Classes\CLSID\{EDECA044-78CD-40EB-8F37-63D947C501A0}\InprocServer32'
function Registered { $k=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($key);if($k){try{$k.GetValue('')}finally{$k.Dispose()}} }
function MachineRegistered { $k=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($key);if($k){try{$k.GetValue('')}finally{$k.Dispose()}} }
function Assert($condition,$message){if(-not $condition){throw $message}}
function Run($exe,$arguments,$label){
 $p=Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
 if(-not $p.WaitForExit(60000)){throw "Timed out: $label (pid $($p.Id))"}
 $code=$p.ExitCode;Write-Output "$label exit=$code";return $code
}
Assert (-not (Get-Process PotPlayer*,NvofControl -ErrorAction SilentlyContinue)) 'Player or controller is running.'
Assert (-not (Test-Path -LiteralPath $destination)) 'QA destination already exists.'
$original=Registered;$machine=MachineRegistered
Assert ($original -and (Test-Path -LiteralPath $original)) 'Expected existing recoverable registration.'
$helper=Join-Path (Split-Path $original -Parent) 'NvofRegister.exe'
$oldIni=Join-Path (Split-Path $original -Parent) 'NvofPotPlayer.ini'
$iniHash=(Get-FileHash -LiteralPath $oldIni).Hash
$filterHash=(Get-FileHash -LiteralPath $original).Hash
$results=[ordered]@{originalRegistration=$original;machineRegistration=$machine}
[ordered]@{original=$original;machine=$machine;iniHash=$iniHash;filterHash=$filterHash}|ConvertTo-Json|Set-Content (Join-Path $root 'registration-before.json')
try {
 $args=@('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="'+$destination+'"'))
 $code=Run $Setup ($args+('/LOG="'+(Join-Path $root 'install.log')+'"')) 'install'
 Assert ($code[-1] -eq 0) 'First installation failed.'
 Assert ((Registered) -eq (Join-Path $destination 'NvofPotPlayer.ax')) 'Registration does not point at installed filter.'
 Assert ((MachineRegistered) -eq $machine) 'Machine registration changed.'
 $installedIni=Join-Path $destination 'NvofPotPlayer.ini'
 Assert (((Get-Content $installedIni | Where-Object {$_ -notmatch '^ExperimentalNativeSynthesis='}) -join "`n") -eq ((Get-Content $oldIni | Where-Object {$_ -notmatch '^ExperimentalNativeSynthesis='}) -join "`n")) 'User settings migration differs.'
 Assert ((Get-Content $installedIni -Raw) -match 'ExperimentalNativeSynthesis=1') 'New backend not selected.'
 Assert (-not (Test-Path -LiteralPath (Join-Path $destination 'component-consent.ini'))) 'Obsolete FRUC consent record created.'
 & (Join-Path $destination 'NvofRegister.exe') --verify | Set-Content (Join-Path $root 'registration-verify.txt');Assert ($LASTEXITCODE -eq 0) 'Installed COM property page failed.'
 & (Join-Path $NativeDirectory 'runtime_dependencies.exe') $destination | Set-Content (Join-Path $root 'runtime-verify.txt');Assert ($LASTEXITCODE -eq 0) 'App-local runtime audit failed.'
 $ui=Run (Join-Path $destination 'NvofControl.exe') @('--self-test','--runtime-info',('"'+(Join-Path $root 'ui-runtime.txt')+'"')) 'controller-self-test';Assert ($ui[-1] -eq 0) 'Controller self-test failed.'
 $results.installWithoutFRUCConsent=$true
 $results.installMigrationRegistrationRuntimeAndUI=$true
 $oldRuntime=Join-Path $destination 'runtime'
 New-Item -ItemType Directory -Path $oldRuntime -Force | Out-Null
 foreach($name in @('NvOFFRUC.dll','NvofFrucBridge.dll','NVEncNVOFFRUC.dll','cudart64_110.dll','cudart64_12.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')){
  Set-Content -LiteralPath (Join-Path $oldRuntime $name) -Value 'Obsolete runtime upgrade fixture'
 }
 Set-Content -LiteralPath (Join-Path $oldRuntime 'keep-user-file.txt') -Value 'Preserve unrelated file'

 Add-Content -LiteralPath (Join-Path $destination 'NvofPotPlayer.ini') -Value "; installer QA preservation marker`nGpuMidpointCorrection=0`nAppearanceProtection=1"
 $changed=(Get-FileHash (Join-Path $destination 'NvofPotPlayer.ini')).Hash
 $code=Run $Setup ($args+('/LOG="'+(Join-Path $root 'upgrade.log')+'"')) 'upgrade';Assert ($code[-1] -eq 0) 'Upgrade failed.'
 Assert ((Get-FileHash (Join-Path $destination 'NvofPotPlayer.ini')).Hash -eq $changed) 'Upgrade modified settings.'
 Assert ((Get-FileHash (Join-Path $destination 'NvofPotPlayer.ini.bak')).Hash -eq $changed) 'Upgrade backup differs.'
 foreach($name in @('NvOFFRUC.dll','NvofFrucBridge.dll','NVEncNVOFFRUC.dll','cudart64_110.dll','cudart64_12.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')){
  Assert (-not (Test-Path -LiteralPath (Join-Path $oldRuntime $name))) "Upgrade retained obsolete runtime $name"
 }
 Assert (Test-Path -LiteralPath (Join-Path $oldRuntime 'keep-user-file.txt')) 'Upgrade removed unrelated runtime file.'
 $results.upgradeRemovesKnownOldRuntimeOnly=$true
 $results.upgradePreservesINI=$true
 $code=Run (Join-Path $destination 'unins000.exe') @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="'+(Join-Path $root 'uninstall.log')+'"')) 'uninstall'
 Assert ($code[-1] -eq 0) 'Uninstall failed.'
 Assert (-not (Registered)) 'Uninstall reactivated an older filter.'
 Assert (Test-Path (Join-Path $destination 'NvofPotPlayer.ini')) 'Uninstall removed settings.'
 Assert (-not (Test-Path (Join-Path $destination 'NvofPotPlayer.ax'))) 'Uninstall left app binary.'
 $results.uninstallRemovesOwnedRegistrationAndPreservesINI=$true
 $code=Run $Setup ($args+('/LOG="'+(Join-Path $root 'reinstall.log')+'"')) 'reinstall';Assert ($code[-1] -eq 0) 'Reinstall failed.'
 & $helper --register | Out-Null;Assert ($LASTEXITCODE -eq 0) 'Cannot restore original registration before ownership test.'
 & (Join-Path $destination 'NvofRegister.exe') --unregister | Out-Null;Assert ($LASTEXITCODE -eq 14) 'Foreign registration ownership guard did not refuse.'
 Assert ((Registered) -eq $original) 'Helper removed another folder registration.'
 $code=Run (Join-Path $destination 'unins000.exe') @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') 'uninstall-other-registration';Assert ($code[-1] -eq 0) 'Ownership uninstall failed.'
 Assert ((Registered) -eq $original) 'Uninstaller removed another folder registration.'
 $results.uninstallDoesNotRemoveOtherRegistration=$true
} finally {
 if((Registered) -ne $original){& $helper --register | Out-Null}
 Assert ((Registered) -eq $original) 'RESTORATION FAILED.'
 Assert ((MachineRegistered) -eq $machine) 'Machine registration mismatch.'
 Assert ((Get-FileHash -LiteralPath $oldIni).Hash -eq $iniHash) 'Original INI changed.'
 Assert ((Get-FileHash -LiteralPath $original).Hash -eq $filterHash) 'Original filter changed.'
 $results.existingInstallationRestored=$true
 $results|ConvertTo-Json|Set-Content (Join-Path $root 'installer-qa.json')
 $results|ConvertTo-Json
}
\n# Expected ownership refusal above is a passing check, not the script exit code.\n$global:LASTEXITCODE=0\n