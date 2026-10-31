# Stage this directory ONLY under C:/trp-work/runs/pascal-sm-general-20261031.
# The manifest pins every executable, pack and DLL. No deployment or game launch.
param([int]$WaitSeconds=3600, [switch]$SkipProbe)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$manifest=Get-Content -LiteralPath (Join-Path $root 'inputs.json') -Raw | ConvertFrom-Json
foreach($file in $manifest.files) {
    $path=Join-Path $root $file.path
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {throw "Input changed: $path"}
}
$results=Join-Path $root ('results/'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $results | Out-Null
# A private executable path lets us identify our fixtures without matching other lanes.
$bin=Join-Path $results 'bin'
New-Item -ItemType Directory -Path $bin | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'TRPNeuralPeripheralBenchmark.exe') -Destination $bin
if(!$SkipProbe) {Copy-Item -LiteralPath (Join-Path $root 'sm-general-probe.exe') -Destination $bin}
function Locked([string]$tag,[string]$command,[string]$log,[string]$success) {
    # Clear inherited developer instrumentation in every child process.
    $clear=''
    foreach($name in 'TRP_NR_KERNEL_PROFILE','TRP_NR_KERNEL_BARRIERS','TRP_NR_LAUNCH_OVERRIDE','TRP_NR_KERNEL_CAPTURE','TRP_NR_KERNEL_PARAMS','TRP_NR_CAPTURE_FATBIN','TRP_NR_ADA_KERNELS') {$clear+='set "'+$name+'="&& '}
    $command=$clear+$command
    # run-in-desktop resolves -Log relative to C:/trp-work, even if an absolute
    # path is supplied. Pass an explicitly checked relative path to that helper.
    $prefix='C:\trp-work\'
    $absoluteLog=[IO.Path]::GetFullPath($log)
    if(!$absoluteLog.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {throw "Log outside test workspace: $log"}
    $relativeLog=$absoluteLog.Substring($prefix.Length)
    $previous=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        $output=& powershell -NoProfile -ExecutionPolicy Bypass -File C:/trp-work/gpu-lock.ps1 -Tag "pascal-sm-general/$tag" -Desktop -WaitSeconds $WaitSeconds -TimeoutSeconds 180 -Log $relativeLog -WorkingDirectory $root -Command $command 2>&1
        $code=$LASTEXITCODE
    } finally {$ErrorActionPreference=$previous}
    $output | Write-Output
    # The lock helper stops its owned command tree on timeout. Do not stop any
    # process here after lock release: another lane may already have acquired it.
    if($code -ne 0 -or ($output -join "`n") -notmatch 'exit code 0; output in') {throw "Locked fixture failed or timed out: $tag. Inspect owned paths; do not retry automatically."}
    $text=Get-Content -LiteralPath $log -Raw
    if($text -notmatch $success -or $text -match '(?m)^FAIL') {throw "Incomplete fixture: $tag"}
    $alive=Get-CimInstance Win32_Process | Where-Object {$_.ExecutablePath -and $_.ExecutablePath.StartsWith($bin+'\',[StringComparison]::OrdinalIgnoreCase)}
    if($alive) {throw "Owned fixture still alive after locked call: $tag. Stop the campaign; do not interfere with other lanes."}
}
if(!$SkipProbe) {Locked 'allocator-probe' ('"'+$bin+'/sm-general-probe.exe"') (Join-Path $results 'allocator-probe.log') '(?m)^PASS 288 cases'}
$records=@()
foreach($size in '768x432','1130x636') {
    $w,$h=$size -split 'x'
    foreach($kind in 'timing','quality') {
        $first=$null
        foreach($round in 1,2) {
            foreach($version in 'old','new') {
                $tag="$size-$kind-$version$round"
                $image=Join-Path $results "$tag.f4"
                $frames=Join-Path $results "$tag-frames.f4"
                $log=Join-Path $results "$tag.log"
                $pack=if($version -eq 'old') {'c37-launch.trpk'} else {'sm-general-pascal.trpk'}
                $command='set "TRP_NR_PASCAL_RESEARCH=1"&& set "TRP_NR_PASCAL_PACK='+$root+'/'+$pack+'"&& "'+$bin+'/TRPNeuralPeripheralBenchmark.exe" "'+$root+'/nvngx_dlssnr.dll" native '+$w+' '+$h+' 0 "'+$image+'"'
                if($kind -eq 'quality') {$command+=' --frames "'+$frames+'"'}
                Locked $tag $command $log $(if($kind -eq 'quality') {'(?m)^QUALITY '} else {'(?m)^RESULT '})
                if((Get-Item -LiteralPath $image).Length -ne ([long]$w*[long]$h*16)) {throw "Truncated final RGBA float32 image: $image"}
                $compare=if($kind -eq 'quality') {$frames} else {$image}
                if($kind -eq 'quality' -and (Get-Item -LiteralPath $frames).Length -ne ([long]40*$w*$h*16)) {throw "Incomplete 40-frame export: $frames"}
                if(!$first) {$first=$compare}
                & fc.exe /b $first $compare > (Join-Path $results "$tag-byte-compare.log")
                if($LASTEXITCODE -ne 0) {throw "RGBA byte parity failed: $first / $compare"}
                $text=Get-Content -LiteralPath $log -Raw
                $records += [pscustomobject]@{size=$size;kind=$kind;version=$version;round=$round;sha256=(Get-FileHash -LiteralPath $compare -Algorithm SHA256).Hash;byteIdentical=$true;result=([regex]::Match($text,'(?m)^(RESULT|QUALITY) .*')).Value}
                $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $results 'results.json')
            }
        }
    }
}
"PASS interleaved plain timing/final-image parity and separate untimed 40-frame RGBA byte parity. Results: $results"
