[CmdletBinding()]
param([int]$Device = -1)
$ErrorActionPreference = 'Stop'
# Use the host's own modules even when launched from a PowerShell 7 process.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Management/Microsoft.PowerShell.Management.psd1') -ErrorAction Stop
$probeRoot = $PSScriptRoot
$probeExe = Join-Path $probeRoot 'dlss5nr_memory_probe.exe'
if (-not (Test-Path -LiteralPath $probeExe)) { throw "Extract the complete diagnostic folder first: $probeExe" }
$runRoot = Join-Path $probeRoot ('results-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$summary = @()
$modules = Get-ChildItem -LiteralPath (Join-Path $probeRoot 'runtime') -File
$modules += Get-Item -LiteralPath $probeExe
$hip = Join-Path $env:WINDIR 'System32/amdhip64_7.dll'
if (Test-Path -LiteralPath $hip) { $modules += Get-Item -LiteralPath $hip }
$modules | ForEach-Object {
    [pscustomobject]@{ Path=$_.FullName; SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash; FileVersion=$_.VersionInfo.FileVersion }
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $runRoot 'module-identities.json') -Encoding UTF8
Write-Host 'Private C10 memory diagnostic. Runs outside Skyrim; keep the integrated GPU configuration as it was for C9.'
Write-Host "Results: $runRoot"
try {
    $cases = @([pscustomobject]@{Name='list'; Lookup='export'; Bytes=1048576; List=$true})
    foreach ($lookup in @('export','gpa')) {
        $cases += [pscustomobject]@{Name='d8'; Lookup=$lookup; Bytes=1048576; List=$false}
        foreach ($case in @('d8','d16','d32','a8','a16','a32')) {
            $cases += [pscustomobject]@{Name=$case; Lookup=$lookup; Bytes=147719680; List=$false}
        }
    }
    foreach ($case in $cases) {
        $label = if ($case.List) { '00-devices' } else { '{0}-{1}-{2}' -f $case.Lookup,$case.Name,$case.Bytes }
        $caseRoot = Join-Path $runRoot $label
        New-Item -ItemType Directory -Path $caseRoot | Out-Null
        $arguments = @(if ($case.List) { '--list' } else { '--case'; $case.Name; '--lookup'; $case.Lookup; '--bytes'; "$($case.Bytes)" })
        if ($Device -ge 0) { $arguments += @('--device',"$Device") }
        Write-Host "Running $label"
        $process = Start-Process -FilePath $probeExe -ArgumentList $arguments -WorkingDirectory $caseRoot -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $caseRoot 'stdout.log') -RedirectStandardError (Join-Path $caseRoot 'stderr.log')
        # Cache the child handle before exit so Windows PowerShell retains its exit code.
        $null = $process.Handle
        if (-not $process.WaitForExit(180000)) {
            # Stop only the child process started above; retain its flushed evidence.
            $process.Kill()
            $process.WaitForExit()
            $summary += [pscustomobject]@{Case=$label; PID=$process.Id; Outcome='timeout'; ExitCode=$null}
            Write-Host 'Probe timed out; remaining cases stopped.'
            break
        }
        $process.WaitForExit()
        $code = $process.ExitCode
        $outcome = if ($code -eq 0) { 'pass' } elseif ($code -eq 3) { 'unsupported' } else { 'failed' }
        $summary += [pscustomobject]@{Case=$label; PID=$process.Id; Outcome=$outcome; ExitCode=$code}
        if ($outcome -eq 'failed') { Write-Host 'Probe failed/crashed; remaining cases stopped. Keep this entire results folder.'; break }
    }
} finally {
    ConvertTo-Json -InputObject @($summary) -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'summary.json') -Encoding UTF8
    Get-ChildItem -LiteralPath $runRoot -File -Recurse | Where-Object Name -ne 'SHA256.csv' | ForEach-Object {
        [pscustomobject]@{ File=$_.FullName.Substring($runRoot.Length+1); SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash }
    } | Export-Csv -LiteralPath (Join-Path $runRoot 'SHA256.csv') -NoTypeInformation
    Write-Host "Send the entire results folder: $runRoot"
}
