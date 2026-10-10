param([int]$Grid=4,[ValidateSet('medium','slow')][string]$Quality='medium',[int]$Dimension=1920,[switch]$Only4K)
$ErrorActionPreference='Stop'
$native=Join-Path (Get-Location) 'build/flow1/native/Release'
$profile="g$Grid-$Quality-$Dimension"
$profileRoot=Join-Path 'build/flow1/clips' $profile
New-Item -ItemType Directory -Path $profileRoot -Force | Out-Null
if(-not $Only4K){
 & (Join-Path $native 'native_motion_layers.exe') 'build/flow1/absent-runtime' 0 $Grid $Quality *> ($profileRoot+'/layers.log')
 if($LASTEXITCODE -ne 0){Write-Output 'Synthetic regression failed; retaining evidence';Get-Content ($profileRoot+'/layers.log') -Tail 4}
}
$cases=@(@('heldout-90','f1-boundaries','p010',3840,1604),@('heldout-130','f1-occlusion','p010',3840,1604),@('jishou-320','jishou-04-320','nv12',1920,1080),@('title','f1-layers/titles','p010',3840,1604),@('kokoore','kokoore-80','nv12',1920,1080),@('hair','jishou-hair','nv12',1920,1080))
foreach($case in $cases){
 if($Only4K -and $case[3] -ne 3840){continue}
 $base=Join-Path 'build/native-synthesis' $case[1]
 $dest=Join-Path $profileRoot $case[0]
 $held=$case[0].StartsWith('heldout')
 $inputName=if($held){'holdout'}else{'input'}
 $times=if($held){'holdout-times.txt'}else{'timestamps.txt'}
 $duration=if($held){834167}else{417083}
 if(Test-Path -LiteralPath ($dest+'/metrics.json')){throw 'Completed evidence already exists'}
 New-Item -ItemType Directory -Path $dest -Force | Out-Null
 & (Join-Path $native 'video_x2_repro.exe') 'build/flow1/absent-runtime' ($base+'/'+$inputName+'.'+$case[2]) ($base+'/'+$times) $dest native $duration $case[2] $case[3] $case[4] $Dimension 0 $Grid $Quality *> ($dest+'/run.log')
 if($LASTEXITCODE -ne 0){Get-Content ($dest+'/run.log') -Tail 5;throw 'Clip failed'}
 & 'C:\Users\mkyoon\AppData\Local\Programs\Python\Python310\python.exe' -X utf8 tools/compare-flow-profile-trial.py $case[0] $dest $case[3] $case[4]
 if($LASTEXITCODE -ne 0){throw 'Comparison failed'}
 $raw=(Resolve-Path -LiteralPath ($dest+'/output.nv12')).ProviderPath
 $allowed=[IO.Path]::GetFullPath('build/flow1/clips')+'\'
 if(-not $raw.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe raw path'}
 if((Get-Item -LiteralPath $raw).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse output'}
 Remove-Item -LiteralPath $raw
}

