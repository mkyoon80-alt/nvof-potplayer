param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
$baseclasses=Join-Path $project 'third_party/windows_samples/Samples/Win7Samples/multimedia/directshow/baseclasses/streams.h'
if (-not (Test-Path -LiteralPath $baseclasses)) {
    throw 'Run tools/fetch-build-deps.ps1 first to acquire the pinned DirectShow baseclasses.'
}
$cmakeCommand=Get-Command cmake -ErrorAction SilentlyContinue
$cmake=if($cmakeCommand){$cmakeCommand.Source}else{$null}
if(-not $cmake){
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if(Test-Path -LiteralPath $vswhere){
        $installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if($installation){$cmake=Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'}
    }
}
if(-not $cmake -or -not (Test-Path -LiteralPath $cmake)){throw 'Install CMake or Visual Studio C++ CMake tools.'}
& $cmake -S $project -B (Join-Path $project 'build') -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed'}
& $cmake --build (Join-Path $project 'build') --config $Configuration --parallel 4
if($LASTEXITCODE -ne 0){throw 'Native build failed'}
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir (Join-Path $project 'build') -C $Configuration --output-on-failure
if($LASTEXITCODE -ne 0){throw 'Core tests failed'}
