param(
    [string]$Version='0.3.1-credits.4',
    [string]$PayloadDirectory,
    [string]$CompilerPath,
    [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
if($Version -notmatch '^\d+\.\d+\.\d+[-.a-zA-Z0-9]*$'){throw 'Invalid version.'}
if(-not $PayloadDirectory){$PayloadDirectory=Join-Path $project "dist/NvofPotPlayer-$Version-win-x64"}
if(-not $CompilerPath){$CompilerPath=Join-Path $project 'build/installer-tools/inno/ISCC.exe'}
if(-not $OutputDirectory){$OutputDirectory=Join-Path $project 'dist'}
$PayloadDirectory=[IO.Path]::GetFullPath($PayloadDirectory)
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$CompilerPath=[IO.Path]::GetFullPath($CompilerPath)
if(-not (Test-Path -LiteralPath $CompilerPath)){throw 'Inno Setup 7.1.0 x64 compiler is required. See docs/BUILD.md.'}
$compilerVersion=(& $CompilerPath --version | Out-String).Trim()
if($compilerVersion -notmatch '\b7\.1\.0\b'){throw "Expected Inno Setup 7.1.0; found $compilerVersion"}
$manifest=Get-Content -LiteralPath (Join-Path $PayloadDirectory 'package-manifest.json') -Raw | ConvertFrom-Json
if($manifest.version -ne $Version){throw 'Payload version mismatch.'}
foreach($required in @('NvofPotPlayer.ax','NvofControl.exe','NvofRegister.exe','coreclr.dll','docs/manual/index.html','docs/manual/manual.css','docs/manual/manual.js','docs/manual/images/controller.png','licenses/NVIDIA-Optical-Flow-SDK-5.0.7-License.pdf','THIRD_PARTY_NOTICES.txt','RELEASE.md')){
    if($required -notin $manifest.files.path){throw "Required payload file missing: $required"}
}
$seen=@{}
foreach($entry in $manifest.files){
    $path=[IO.Path]::GetFullPath((Join-Path $PayloadDirectory $entry.path))
    if(-not $path.StartsWith($PayloadDirectory.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Manifest path escapes payload.'}
    if($seen.ContainsKey($path)){throw 'Duplicate manifest entry.'}
    $seen[$path]=$true
    $item=Get-Item -LiteralPath $path
    if($item.Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256){throw "Payload mismatch: $($entry.path)"}
}
foreach($file in Get-ChildItem -LiteralPath $PayloadDirectory -Recurse -File){
    if($file.FullName -ne (Join-Path $PayloadDirectory 'package-manifest.json') -and -not $seen.ContainsKey($file.FullName)){throw "Unlisted payload file: $($file.Name)"}
    if($file.Name -in @('nvcuda.dll','nvofapi64.dll','nvapi64.dll','NvOFFRUC.h','NvOFFRUC.dll','NvofFrucBridge.dll','NVEncNVOFFRUC.dll','cudart64_110.dll','cudart64_12.dll')){throw "Forbidden driver/SDK payload: $($file.Name)"}
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$name="NvofPotPlayer-$Version-Setup-x64.exe"
$setup=Join-Path $OutputDirectory $name
if(Test-Path -LiteralPath $setup){throw 'Installer output already exists; choose another output directory. Published files must not be overwritten.'}
& $CompilerPath '--quiet-progress' "--define=AppVersion=$Version" "--define=PayloadDir=$PayloadDirectory" "--define=OutputDir=$OutputDirectory" (Join-Path $project 'installer/NvofPotPlayer.iss')
if($LASTEXITCODE -ne 0){throw "Installer compilation failed: $LASTEXITCODE"}
$hash=(Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $OutputDirectory "$name.sha256"),"$hash  $name`n")
Write-Output $setup
Write-Output "SHA256 $hash"
