param()
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$destination = Join-Path $project 'third_party/windows_samples'
$revision = '434f6002bdf9cf9829406c3ff2b33387982d6168'
$origin = 'https://github.com/microsoft/Windows-classic-samples.git'
function Invoke-Git([string[]]$Arguments) {
    & git @Arguments
    if ($LASTEXITCODE -ne 0) { throw ('git failed: ' + ($Arguments -join ' ')) }
}
if (Test-Path -LiteralPath (Join-Path $destination '.git')) {
    $actual = & git -C $destination rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actual -ne $revision) {
        throw 'Existing DirectShow checkout is not the pinned revision. Preserve your changes and use a clean checkout.'
    }
    $dirty = & git -C $destination status --porcelain
    if ($dirty) { throw 'Existing DirectShow checkout has local changes.' }
    Write-Host 'Pinned DirectShow build dependency is ready.'
    exit 0
}
if (Test-Path -LiteralPath $destination) { throw 'Dependency directory already exists without Git metadata; no files were changed.' }
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Invoke-Git @('init', $destination)
Invoke-Git @('-C', $destination, 'remote', 'add', 'origin', $origin)
Invoke-Git @('-C', $destination, 'config', 'core.sparseCheckout', 'true')
$sparse = "Samples/Win7Samples/multimedia/directshow/baseclasses/`n/LICENSE`n"
[IO.File]::WriteAllText((Join-Path $destination '.git/info/sparse-checkout'), $sparse)
Invoke-Git @('-C', $destination, 'fetch', '--depth=1', '--filter=blob:none', 'origin', $revision)
Invoke-Git @('-C', $destination, 'checkout', '--detach', $revision)
Write-Host 'Pinned Microsoft DirectShow baseclasses acquired.'
