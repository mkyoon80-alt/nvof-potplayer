param([switch]$Apply,[string]$Version='0.2.0-preview.8')
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
if($Version -notmatch '^\d+\.\d+\.\d+[-.a-zA-Z0-9]*$'){throw 'Invalid version'}
$package=Join-Path $project ('dist/NvofPotPlayer-'+$Version+'-win-x64')
$install=Join-Path $project 'bin'
$manifest=Get-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Raw | ConvertFrom-Json
$plan=@()
foreach($entry in $manifest.files){
 $relative=$entry.path.Replace('/','\')
 $source=[IO.Path]::GetFullPath((Join-Path $package $relative))
 $target=[IO.Path]::GetFullPath((Join-Path $install $relative))
 if(-not $source.StartsWith($package+'\',[StringComparison]::OrdinalIgnoreCase) -or -not $target.StartsWith($install+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid package path'}
 if((Get-FileHash -LiteralPath $source).Hash -ne $entry.sha256){throw "Package mismatch: $relative"}
 if($relative -eq 'NvofPotPlayer.ini'){continue}
 $existing=Test-Path -LiteralPath $target
 $before=if($existing){(Get-FileHash -LiteralPath $target).Hash}else{''}
 if($before -ne $entry.sha256){$plan+=[pscustomobject]@{Relative=$relative;Source=$source;Target=$target;Existing=$existing;Before=$before;After=$entry.sha256}}
}
if(-not $Apply){$plan | Where-Object {$_.Relative -match 'Nvof|runtime'} | Select-Object Relative,Existing; Write-Output "Verified package: $($plan.Count) changed files. Preferences and registration will be preserved."; return}
if(@(Get-Process PotPlayer64,PotPlayerMini64,NvofControl -ErrorAction SilentlyContinue).Count){throw 'Close PotPlayer and NVOF Control first.'}
if(-not $plan.Count){Write-Output 'Package files already match; no installation needed.'; return}
$backup=Join-Path $project ('backup/x2-only-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backup | Out-Null
$ini=Join-Path $install 'NvofPotPlayer.ini'
$iniBefore=(Get-FileHash -LiteralPath $ini).Hash
Copy-Item -LiteralPath $ini -Destination $backup
foreach($item in $plan){
 if($item.Existing){
  $saved=Join-Path $backup $item.Relative
  New-Item -ItemType Directory -Path (Split-Path $saved -Parent) -Force | Out-Null
  Copy-Item -LiteralPath $item.Target -Destination $saved
 }
}
$plan | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'files.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'restore-x2-update.ps1') -Destination (Join-Path $backup 'restore.ps1')
try{
 foreach($item in $plan){
  New-Item -ItemType Directory -Path (Split-Path $item.Target -Parent) -Force | Out-Null
  Copy-Item -LiteralPath $item.Source -Destination $item.Target -Force
  if((Get-FileHash -LiteralPath $item.Target).Hash -ne $item.After){throw "Installed checksum mismatch: $($item.Relative)"}
 }
 if((Get-FileHash -LiteralPath $ini).Hash -ne $iniBefore){throw 'Existing preferences changed unexpectedly'}
 & (Join-Path $install 'NvofRegister.exe') --verify
 if($LASTEXITCODE -ne 0){throw 'Existing registration did not verify'}
}catch{ & (Join-Path $backup 'restore.ps1'); throw }
Write-Output "Installed $Version. Backup: $backup"
Write-Output 'Player settings preserved. Legacy output keys are ignored and cleaned when the controller opens.'
