param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
if(-not $BuildDirectory){$BuildDirectory=Join-Path $project 'build/phase-upgrade/Release'}
if(@(Get-Process PotPlayer64,PotPlayerMini64,NvofControl -ErrorAction SilentlyContinue).Count){throw 'Close PotPlayer and NVOF Control first.'}
$install=Join-Path $project 'bin'
$filter=Join-Path $BuildDirectory 'NvofPotPlayer.ax'
$bridge=Join-Path $BuildDirectory 'NvofFrucBridge.dll'
foreach($file in @($filter,$bridge)){if(-not (Test-Path -LiteralPath $file)){throw "Build artifact missing: $file"}}
$runtime=Join-Path $install 'runtime'
$manifest=Get-Content (Join-Path $project 'config/runtime-manifest.json') -Raw | ConvertFrom-Json
$missingRuntimes=@()
foreach($entry in $manifest.files | Where-Object {$_.name -ne 'NVEncNVOFFRUC.dll'}){
 $source=Join-Path $project ('runtime/'+$entry.name)
 if(-not (Test-Path -LiteralPath $source) -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $entry.sha256){throw "Runtime source differs from tested version: $($entry.name)"}
 $file=Join-Path $runtime $entry.name
 if(Test-Path -LiteralPath $file){
  if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256){throw "Installed runtime differs from tested version: $($entry.name)"}
 } else {$missingRuntimes+=$source}
}
$backup=Join-Path $project ('backup/phase-upgrade-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
foreach($name in @('NvofPotPlayer.ax','NvofControl.exe','NvofPotPlayer.ini')){
 $file=Join-Path $install $name
 if(Test-Path -LiteralPath $file){Copy-Item -LiteralPath $file -Destination $backup}
}
$bridgeTarget=Join-Path $runtime 'NvofFrucBridge.dll'
if(Test-Path -LiteralPath $bridgeTarget){Copy-Item -LiteralPath $bridgeTarget -Destination $backup}
$rollback=@'
$ErrorActionPreference='Stop'
if(@(Get-Process PotPlayer64,PotPlayerMini64,NvofControl -ErrorAction SilentlyContinue).Count){throw 'Close PotPlayer and NVOF Control first.'}
$project=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NvofPotPlayer.ax') -Destination (Join-Path $project 'bin/NvofPotPlayer.ax') -Force
if(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'NvofFrucBridge.dll')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NvofFrucBridge.dll') -Destination (Join-Path $project 'bin/runtime/NvofFrucBridge.dll') -Force}
Write-Output 'Previous filter restored. Current playback preferences were preserved.'
'@
[IO.File]::WriteAllText((Join-Path $backup 'restore.ps1'),$rollback)
try {
 foreach($source in $missingRuntimes){Copy-Item -LiteralPath $source -Destination $runtime}
 Copy-Item -LiteralPath $bridge -Destination $bridgeTarget -Force
 Copy-Item -LiteralPath $filter -Destination (Join-Path $install 'NvofPotPlayer.ax') -Force
 foreach($pair in @(@($filter,(Join-Path $install 'NvofPotPlayer.ax')),@($bridge,$bridgeTarget))){
  if((Get-FileHash -LiteralPath $pair[0]).Hash -ne (Get-FileHash -LiteralPath $pair[1]).Hash){throw 'Installed artifact checksum mismatch'}
 }
} catch {
 Copy-Item -LiteralPath (Join-Path $backup 'NvofPotPlayer.ax') -Destination (Join-Path $install 'NvofPotPlayer.ax') -Force
 throw
}
Write-Output "Installed independent motion phases. Backup: $backup"
Write-Output 'Player settings, controller and COM registration were preserved.'
