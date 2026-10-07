param([string]$ToolsDirectory = (Join-Path $PSScriptRoot '..\build\.tools'))
$ErrorActionPreference = 'Stop'
$pin = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dotnet-sdk.json') | ConvertFrom-Json
$toolsPath = [IO.Path]::GetFullPath($ToolsDirectory)
$sdkPath = Join-Path $toolsPath ('dotnet-' + $pin.SDK)
$executable = Join-Path $sdkPath 'dotnet.exe'
if (Test-Path -LiteralPath $executable) { return $executable }
$uri = [Uri]$pin.Url
if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'builds.dotnet.microsoft.com') { throw 'The SDK pin must use the official Microsoft download host.' }
New-Item -ItemType Directory -Force -Path $toolsPath | Out-Null
$archive = Join-Path $toolsPath ('dotnet-sdk-' + $pin.SDK + '-win-x64.zip')
$ProgressPreference = 'SilentlyContinue'
if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $uri.AbsoluteUri -OutFile $archive }
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA512).Hash -ne $pin.SHA512) { throw 'Microsoft SDK archive SHA512 mismatch. The archive was not extracted.' }
Expand-Archive -LiteralPath $archive -DestinationPath $sdkPath
if (-not (Test-Path -LiteralPath $executable)) { throw 'The verified SDK archive did not contain dotnet.exe.' }
return $executable
