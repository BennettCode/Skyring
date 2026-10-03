<#
.SYNOPSIS
  Copies both games' save folders into local/save-backups/<timestamp>/ and keeps the newest -Keep backups.
#>
[CmdletBinding()]
param([int]$Keep = 10)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force

$paths = Get-ProjectPaths
$dest = Join-Path $paths.SaveBackups (Get-Timestamp)

foreach ($pair in @(@{ Name = 'eldenring'; Src = $paths.EldenRingSaves }, @{ Name = 'skyrim'; Src = $paths.SkyrimSaves })) {
    if (-not (Test-Path -LiteralPath $pair.Src)) { Write-Warning "save folder not found: $($pair.Src)"; continue }
    $target = Join-Path $dest $pair.Name
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item -Path (Join-Path $pair.Src '*') -Destination $target -Recurse -Force
    $count = @(Get-ChildItem -LiteralPath $target -Recurse -File).Count
    Write-Host "backed up $count file(s) from $($pair.Src)"
}
Write-Host "backup: $dest" -ForegroundColor Green

$all = @(Get-ChildItem -LiteralPath $paths.SaveBackups -Directory | Sort-Object Name -Descending)
if ($all.Count -gt $Keep) {
    $all | Select-Object -Skip $Keep | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Recurse -Force
        Write-Host "pruned old backup $($_.Name)"
    }
}
