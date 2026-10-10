param(
    [string]$Version='0.3.1-standalone.1',
    [string]$UiDirectory,
    [string]$NativeDirectory,
    [string]$RuntimeDirectory,
    [string]$OutputRoot
)
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
if($Version -notmatch '^\d+\.\d+\.\d+[-.a-zA-Z0-9]*$'){throw 'Invalid version.'}
if(-not $UiDirectory){$UiDirectory=Join-Path $project 'ui/staging-selfcontained'}
if(-not $NativeDirectory){$NativeDirectory=Join-Path $project 'build/Release'}
foreach($name in @('NvofControl.exe','NvofControl.runtimeconfig.json','coreclr.dll','hostfxr.dll','PresentationFramework.dll')){
    if(-not (Test-Path -LiteralPath (Join-Path $UiDirectory $name))){throw "Self-contained UI file missing: $name"}
}
# A publish folder must contain only runtime assemblies, the app host and manifests.
# Refuse personal settings, local logs or preview images instead of copying them.
foreach($file in Get-ChildItem -LiteralPath $UiDirectory -File -Recurse -Force){
    $relative=$file.FullName.Substring(([IO.Path]::GetFullPath($UiDirectory)).TrimEnd('\').Length+1).Replace('\','/')
    $allowed=$file.Extension -eq '.dll' -or $relative -in @('NvofControl.exe','createdump.exe','NvofControl.deps.json','NvofControl.runtimeconfig.json')
    if(-not $allowed){throw "Unexpected UI publish file: $relative. Use a clean self-contained publish directory."}
}
$runtimeConfig=Get-Content -LiteralPath (Join-Path $UiDirectory 'NvofControl.runtimeconfig.json') -Raw | ConvertFrom-Json
if($runtimeConfig.runtimeOptions.framework -or $runtimeConfig.runtimeOptions.frameworks){throw 'UI is framework-dependent; publish self-contained first.'}
foreach($name in @('NvofPotPlayer.ax','NvofRegister.exe')){
    if(-not (Test-Path -LiteralPath (Join-Path $NativeDirectory $name))){throw "Native build missing: $name"}
}
foreach($name in @('LICENSE','THIRD_PARTY_NOTICES.md','licenses/README.md','config/NvofPotPlayer.ini','RELEASE.md','docs/manual/index.html','licenses/NVIDIA-Optical-Flow-SDK-5.0.7-License.pdf')){
    if(-not (Test-Path -LiteralPath (Join-Path $project $name))){throw "Package document missing: $name"}
}
$name="NvofPotPlayer-$Version-win-x64"
$dist=if($OutputRoot){[IO.Path]::GetFullPath($OutputRoot)}else{Join-Path $project 'dist'}
$destination=Join-Path $dist $name
$archive=Join-Path $dist "$name.zip"
if((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath $archive)){throw 'Package output already exists. Choose a new version; existing packages are preserved.'}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Get-ChildItem -LiteralPath $UiDirectory -Force | Where-Object {$_.Extension -ne '.pdb'} | Copy-Item -Destination $destination -Recurse
foreach($file in @('NvofPotPlayer.ax','NvofRegister.exe')){Copy-Item -LiteralPath (Join-Path $NativeDirectory $file) -Destination $destination}
Copy-Item -LiteralPath (Join-Path $project 'config/NvofPotPlayer.ini') -Destination $destination
foreach($file in @('LICENSE','THIRD_PARTY_NOTICES.md')){Copy-Item -LiteralPath (Join-Path $project $file) -Destination $destination}
$licenseOutput=Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenseOutput -Force | Out-Null
# Keep SDK terms and current runtime notices; old FRUC/CUDA bundle notices are historical.
foreach($file in Get-ChildItem -LiteralPath (Join-Path $project 'licenses') -File){
    if($file.Name -notin @('NVIDIA-COMPONENT-TERMS.txt','NVIDIA-CUDA-11.2-EULA.html','NVEnc-MIT.txt')){
        Copy-Item -LiteralPath $file.FullName -Destination $licenseOutput
    }
}
Copy-Item -LiteralPath (Join-Path $project 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $destination 'THIRD_PARTY_NOTICES.txt')
$manualOutput=Join-Path $destination 'docs/manual'
New-Item -ItemType Directory -Path $manualOutput -Force | Out-Null
Copy-Item -Path (Join-Path $project 'docs/manual/*') -Destination $manualOutput -Recurse
Copy-Item -LiteralPath (Join-Path $project 'RELEASE.md') -Destination $destination
Copy-Item -LiteralPath (Join-Path $project 'docs/STANDALONE.md') -Destination (Join-Path $destination 'docs')
$configOutput=Join-Path $destination 'config'
New-Item -ItemType Directory -Path $configOutput -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $project 'config/runtime-manifest.json') -Destination $configOutput
Copy-Item -LiteralPath (Join-Path $project 'ui/dotnet-sdk.json') -Destination $configOutput
Copy-Item -LiteralPath (Join-Path $project 'docs/POTPLAYER_SETUP.ko.md') -Destination (Join-Path $destination 'START-HERE.ko.md')
foreach($file in Get-ChildItem -LiteralPath $destination -Recurse -File){
    if($file.Name -in @('NvOFFRUC.dll','NvofFrucBridge.dll','NVEncNVOFFRUC.dll','nvcuda.dll','nvofapi64.dll') -or $file.Name -like 'cudart*.dll'){
        throw "Forbidden runtime in native payload: $($file.Name)"
    }
}
$entries=Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($destination.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$sourceCommit=(& git -C $project rev-parse HEAD).Trim()
$sourceDirty=[bool](& git -C $project status --porcelain)
$manifest=[ordered]@{sourceCommit=$sourceCommit;sourceDirty=$sourceDirty;name='NVOF for PotPlayer';version=$Version;platform='win-x64';distribution='Self-contained native filter and controller; no FRUC/CUDA or GPU driver binaries; see THIRD_PARTY_NOTICES.txt';files=@($entries)}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'package-manifest.json') -Encoding UTF8
Compress-Archive -LiteralPath $destination -DestinationPath $archive -CompressionLevel Optimal
$hash=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $dist "$name.sha256"),"$hash  $name.zip`n")
Write-Output $archive
Write-Output "SHA256 $hash"
Write-Output 'Local complete package built. No files were registered, installed, or uploaded.'
