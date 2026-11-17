param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$EvidenceRoot,
    [Parameter(Mandatory)][string]$Name,
    [ValidateSet('auto','classic','force','bad-preimage','preloaded')][string]$Mode = 'auto',
    [ValidateSet('timing','proof','debug','recreate')][string]$Kind = 'timing',
    [int]$InputWidth = 1280,
    [int]$InputHeight = 720,
    [int]$Frames = 240,
    [Parameter(Mandatory)][string]$AMDDirectory,
    # Optional wrapper that serializes GPU work: it is called as <wrapper> -Tag <tag> -Command <cmd> ...
    [string]$GpuLock = $env:TRP_GPU_LOCK,
    [int]$ExpectedExit = 0
)
$ErrorActionPreference = 'Stop'
$official = $AMDDirectory
$expected = @{
    'amd_fidelityfx_loader_dx12.dll' = 'E2191D3C0828BE45EB8AFFD4E3EC0378F2293ADC345B772A7170EDCF3B021ECB'
    'amd_fidelityfx_upscaler_dx12.dll' = 'AAD4FCCCCB29084A655F9A4F9F613BD22C8C03FB4A0F8840148CC6543B4CCACE'
}
function RuntimeIdentity {
    foreach ($file in $expected.Keys | Sort-Object) {
        $path = Join-Path $official $file
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($hash -ne $expected[$file]) { throw "Official runtime changed: $file" }
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        if ($signature.Status -ne 'Valid') { throw "Official signature is not valid: $file" }
        [pscustomobject]@{path=$path;sha256=$hash;signature=[string]$signature.Status;signer=$signature.SignerCertificate.Subject;version=(Get-Item -LiteralPath $path).VersionInfo.FileVersion}
    }
}
if ($Name -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Use a simple case name' }
$caseDirectory = Join-Path $EvidenceRoot $Name
if (Test-Path -LiteralPath $caseDirectory) { throw 'Case evidence already exists; do not overwrite a run' }
if (Test-Path -LiteralPath (Join-Path $EvidenceRoot 'STOP-GPU-FAILURE.txt')) { throw 'A prior GPU/forced-provider fault requires review; no automatic retry' }
New-Item -ItemType Directory -Path $caseDirectory | Out-Null
@(RuntimeIdentity) | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $caseDirectory 'runtime-before.json')
$command = '"{0}" "{1}" "{2}" {3} {4} {5} {6} {7}' -f $Executable,$AMDDirectory,$caseDirectory,$Mode,$InputWidth,$InputHeight,$Kind,$Frames
$exitCode = 1
try {
    if (-not $GpuLock) { throw 'Set -GpuLock or TRP_GPU_LOCK to the GPU lock wrapper script' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $GpuLock -Tag fsr4force -Command $command -WorkingDirectory (Split-Path -Parent $Executable) -TimeoutSeconds 180 -WaitSeconds 60 2>&1 |
        Tee-Object -FilePath (Join-Path $caseDirectory 'run.log')
    $exitCode = $LASTEXITCODE
} finally {
    @(RuntimeIdentity) | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $caseDirectory 'runtime-after.json')
    [pscustomobject]@{command=$command;exit_code=$exitCode;expected_exit=$ExpectedExit;utc=[DateTime]::UtcNow.ToString('o');executable_sha256=(Get-FileHash -LiteralPath $Executable).Hash} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $caseDirectory 'receipt.json')
}
if ($exitCode -eq 124 -or $exitCode -lt 0 -or ($Mode -eq 'force' -and $exitCode -notin @(0,20,30))) {
    "Stopped after case $Name, exit $exitCode. No further GPU runs without reviewing the failure." | Set-Content -LiteralPath (Join-Path $EvidenceRoot 'STOP-GPU-FAILURE.txt')
}
if ($exitCode -ne $ExpectedExit) { throw "Case $Name returned $exitCode, expected $ExpectedExit" }
