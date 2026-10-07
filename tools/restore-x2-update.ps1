$ErrorActionPreference='Stop'
if(@(Get-Process PotPlayer64,PotPlayerMini64,NvofControl -ErrorAction SilentlyContinue).Count){throw 'Close PotPlayer and NVOF Control first.'}
$project=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$install=Join-Path $project 'bin'
foreach($item in (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'files.json') -Raw | ConvertFrom-Json)){
 $target=[IO.Path]::GetFullPath((Join-Path $install $item.Relative))
 $saved=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot $item.Relative))
 if(-not $target.StartsWith($install+'\',[StringComparison]::OrdinalIgnoreCase) -or -not $saved.StartsWith($PSScriptRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid restore path'}
 if($item.Existing){Copy-Item -LiteralPath $saved -Destination $target -Force}else{if(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target}}
}
Write-Output 'Previous code and UI restored. Current preferences preserved; original INI is also in this backup.'
