param([Parameter(Mandatory)][string]$SourceDirectory, [Parameter(Mandatory)][string]$AliasPath)
$ErrorActionPreference = 'Stop'
$sourcePath = [IO.Path]::GetFullPath($SourceDirectory)
$aliasDirectory = [IO.Path]::GetFullPath($AliasPath)
if (-not (Test-Path -LiteralPath $sourcePath -PathType Container)) { throw 'Fixture directory is missing.' }
if (Test-Path -LiteralPath $aliasDirectory) {
    $item = Get-Item -LiteralPath $aliasDirectory -Force
    if ($item.LinkType -ne 'Junction' -or [IO.Path]::GetFullPath([string]$item.Target[0]) -ine $sourcePath) {
        throw 'Existing fixture alias has a different target; refusing to replace it.'
    }
} else {
    New-Item -ItemType Junction -Path $aliasDirectory -Target $sourcePath | Out-Null
}
