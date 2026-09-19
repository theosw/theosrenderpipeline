#Requires -Version 7.0
<#
.SYNOPSIS
Adds edition-specific MO2 metadata to an assembled public package before zipping.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][ValidateSet('Standard', 'Universal')][string]$Edition,
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')][string]$Version
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $PackageDirectory).Path
$marker = if ($Edition -eq 'Standard') { 'TRP-STANDARD-PACKAGE.txt' } else { 'TRP-FULL-PACKAGE.txt' }
$files = @('LICENSE', 'README.md', 'THIRD-PARTY.md', $marker)
if ($Edition -eq 'Standard') { $files += 'NVIDIA-LICENSES.txt' }
foreach ($relative in $files + @('SKSE/Plugins/TheosRenderPipeline.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) {
        throw "Missing public package file: $relative"
    }
}
# Refuse a private staging tree: its manifest would need a separate identity update.
foreach ($entry in Get-ChildItem -LiteralPath $root -Force) {
    if ($entry.Name -notin ($files + @('SKSE', 'fomod'))) {
        throw "Unexpected public package entry: $($entry.Name)"
    }
}
$description = if ($Edition -eq 'Standard') {
    'Includes NVIDIA runtimes. Install separately from Universal; do not merge the two editions.'
} else {
    'Install separately below matching Standard in MO2 and enable both, or supply NVIDIA runtimes yourself. Do not merge the two editions.'
}
$templates = Join-Path (Split-Path $PSScriptRoot -Parent) 'package/fomod'
$folder = Join-Path $root 'fomod'
[IO.Directory]::CreateDirectory($folder) | Out-Null
foreach ($name in @('info.xml', 'ModuleConfig.xml')) {
    $xml = Get-Content -LiteralPath (Join-Path $templates "$name.in") -Raw
    $fileEntries = ($files | ForEach-Object { "    <file source=`"$_`" destination=`"$_`" />" }) -join "`n"
    $xml = $xml.Replace('@EDITION@', $Edition).Replace('@VERSION@', $Version).
        Replace('@DESCRIPTION@', $description).Replace('@FILES@', $fileEntries)
    $null = [xml]$xml
    [IO.File]::WriteAllText((Join-Path $folder $name), $xml, [Text.UTF8Encoding]::new($false))
}
Write-Output "Installer name: Theo's Render Pipeline - $Edition (version $Version)"
