param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'staging-selfcontained'),
    [string]$DotNetExecutable = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pin = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dotnet-sdk.json') | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($DotNetExecutable)) { $DotNetExecutable = & (Join-Path $PSScriptRoot 'get-dotnet-sdk.ps1') }
$DotNetExecutable = [IO.Path]::GetFullPath($DotNetExecutable)
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$artifactsPath = Join-Path $projectRoot 'build\ui-dotnet'
$toolsPath = Join-Path $projectRoot 'build\.tools'
New-Item -ItemType Directory -Force -Path $outputPath,$artifactsPath,$toolsPath | Out-Null
$environment = @{
    DOTNET_ROOT = [IO.Path]::GetDirectoryName($DotNetExecutable)
    DOTNET_CLI_HOME = (Join-Path $toolsPath 'dotnet-cli')
    NUGET_PACKAGES = (Join-Path $toolsPath 'nuget-packages')
    NUGET_HTTP_CACHE_PATH = (Join-Path $toolsPath 'nuget-cache')
    DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
    DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
    DOTNET_NOLOGO = '1'
}
$previousEnvironment = @{}
foreach ($name in $environment.Keys) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
}
Push-Location -LiteralPath $PSScriptRoot
try {
    $actualSdk = (& $DotNetExecutable --version).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualSdk -ne $pin.SDK) { throw "Expected .NET SDK $($pin.SDK), got $actualSdk." }
    $arguments = @('publish', (Join-Path $PSScriptRoot 'NvofControl.csproj'), '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true', '-o', $outputPath, '--artifacts-path', $artifactsPath, '--configfile', (Join-Path $PSScriptRoot 'NuGet.Config'), '--disable-build-servers', ('-p:RuntimeFrameworkVersion=' + $pin.Runtime))
    & $DotNetExecutable @arguments
    if ($LASTEXITCODE -ne 0) { throw "Self-contained UI publish failed: $LASTEXITCODE" }
    foreach ($required in @('NvofControl.exe','NvofControl.dll','NvofControl.runtimeconfig.json','coreclr.dll','hostfxr.dll','hostpolicy.dll','PresentationFramework.dll','wpfgfx_cor3.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $outputPath $required))) { throw "Self-contained publish is incomplete: $required" }
    }
    Write-Output (Join-Path $outputPath 'NvofControl.exe')
}
finally {
    Pop-Location
    foreach ($name in $previousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
}
