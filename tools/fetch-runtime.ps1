[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $PSScriptRoot '..\runtime'),
    [string]$NvencArchive,
    [string]$ExtractorPath,
    [string]$VcRedistDirectory,
    [switch]$AcceptThirdPartyLicenses
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifestPath = Join-Path $projectRoot 'config\runtime-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if (-not $AcceptThirdPartyLicenses) {
    throw 'Read licenses/README.md and the vendor agreements first, then use -AcceptThirdPartyLicenses for your local acquisition. This does not authorize public redistribution of NVIDIA runtime DLLs.'
}
if (-not [Environment]::Is64BitOperatingSystem) { throw 'Windows x64 is required.' }
$Destination = [IO.Path]::GetFullPath($Destination)
$cache = Join-Path $projectRoot 'runtime\download\acquisition'
New-Item -ItemType Directory -Path $cache -Force | Out-Null

function Assert-Hash([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing file: $Path" }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $Expected) { throw "SHA256 mismatch for $Path. Expected $Expected; got $actual. Nothing will bypass this check." }
}
function Get-PinnedDownload([string]$Url, [string]$Path, [string]$Hash) {
    if (Test-Path -LiteralPath $Path) { Assert-Hash $Path $Hash; return }
    $part = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.partial'
    Write-Host "Downloading $Url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $part -UseBasicParsing
    Assert-Hash $part $Hash
    Move-Item -LiteralPath $part -Destination $Path
}
function Assert-Runtime([string]$Path, $Entry) {
    Assert-Hash $Path $Entry.sha256
    if ((Get-Item -LiteralPath $Path).Length -ne $Entry.bytes) { throw "Unexpected size: $Path" }
    if ($Entry.signature -eq 'Valid') {
        $signature = Get-AuthenticodeSignature -LiteralPath $Path
        if ($signature.Status -ne 'Valid') { throw "Invalid Authenticode signature: $Path ($($signature.Status))" }
    }
}

if (-not $NvencArchive) {
    $NvencArchive = Join-Path $cache 'NVEncC_9.37_x64.7z'
    Get-PinnedDownload $manifest.archive.url $NvencArchive $manifest.archive.sha256
} else { $NvencArchive = [IO.Path]::GetFullPath($NvencArchive); Assert-Hash $NvencArchive $manifest.archive.sha256 }
if (-not $ExtractorPath) {
    $ExtractorPath = Join-Path $cache '7zr.exe'
    Get-PinnedDownload $manifest.extractor.url $ExtractorPath $manifest.extractor.sha256
} else {
    $ExtractorPath = [IO.Path]::GetFullPath($ExtractorPath)
    if (-not (Test-Path -LiteralPath $ExtractorPath -PathType Leaf)) { throw "Extractor not found: $ExtractorPath" }
    # An explicitly supplied extractor is a user-selected tool. The default
    # downloaded extractor is pinned above, and every extracted DLL is pinned.
}

$stage = Join-Path $cache ('stage-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$archiveEntries = @($manifest.files | Where-Object { $_.acquisition -eq 'nvenc_archive' })
$archiveNames = @($archiveEntries | ForEach-Object { $_.name })
& $ExtractorPath e -y "-o$stage" $NvencArchive @archiveNames | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Extraction failed with exit code $LASTEXITCODE" }
foreach ($entry in $archiveEntries) { Assert-Runtime (Join-Path $stage $entry.name) $entry }

$crtEntries = @($manifest.files | Where-Object { $_.acquisition -eq 'visual_studio_redist' })
if (-not $VcRedistDirectory) {
    # Build-machine lookup only. The completed local ZIP does not need Visual Studio.
    $candidates = @()
    foreach ($programDir in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($programDir) {
            $pattern = Join-Path $programDir 'Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT'
            $candidates += @(Get-Item -Path $pattern -ErrorAction SilentlyContinue | Where-Object { $_.PSIsContainer })
        }
    }
    foreach ($candidate in $candidates) {
        $matches = $true
        foreach ($entry in $crtEntries) {
            $path = Join-Path $candidate.FullName $entry.name
            if (-not (Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { $matches = $false; break }
        }
        if ($matches) { $VcRedistDirectory = $candidate.FullName; break }
    }
    if (-not $VcRedistDirectory) {
        throw 'Pinned Visual C++ x64 CRT 14.42.34438.0 was not found in Visual Studio VC/Redist. Pass -VcRedistDirectory with the licensed matching release CRT folder. Do not copy driver/system DLLs or silently substitute unreviewed files.'
    }
}
foreach ($entry in $crtEntries) {
    $source = Join-Path $VcRedistDirectory $entry.name
    Assert-Runtime $source $entry
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $entry.name)
}
# Validate the whole dependency set and all pre-existing targets before installing.
foreach ($entry in $manifest.files) {
    Assert-Runtime (Join-Path $stage $entry.name) $entry
    $target = Join-Path $Destination $entry.name
    if (Test-Path -LiteralPath $target) { Assert-Runtime $target $entry }
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
foreach ($entry in $manifest.files) {
    $target = Join-Path $Destination $entry.name
    if (-not (Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath (Join-Path $stage $entry.name) -Destination $target }
}
Write-Host 'Verified 6 local runtime DLLs. No toolkit, registry, player or machine-wide runtime installation was changed.'
Write-Host 'Do not upload the acquired proprietary binaries to a public repository or release without the applicable distribution rights.'
