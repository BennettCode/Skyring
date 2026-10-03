<#
.SYNOPSIS
  Copies every relevant log from both games into logs/<timestamp>/ and prints the [core] lines from both plugins.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force

$paths = Get-ProjectPaths
$dest = Join-Path $paths.LogsDir (Get-Timestamp)
New-Item -ItemType Directory -Force $dest | Out-Null

$sources = @(
    (Join-Path $paths.SkyrimLogDir 'SkyrimXER.log'),
    (Join-Path $paths.SkyrimLogDir 'skse64.log'),
    (Join-Path $paths.BuildDir 'er-plugin\logs\skyrimxer_er.log')
)
$sources += @(Get-ChildItem -LiteralPath $paths.SkyrimLogDir -Filter 'crash-*.log' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1 | ForEach-Object FullName)
$sources += @(Get-ChildItem -LiteralPath $paths.LogsDir -Filter 'me3-*.log' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notmatch 'stdout' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1 | ForEach-Object FullName)

foreach ($src in $sources) {
    if ($src -and (Test-Path -LiteralPath $src)) {
        Copy-Item -LiteralPath $src -Destination $dest
        Write-Host ("collected {0,-22} ({1})" -f (Split-Path -Leaf $src), (Get-Item -LiteralPath $src).LastWriteTime)
    } elseif ($src) {
        Write-Host "missing   $src" -ForegroundColor DarkYellow
    }
}

Write-Host "`n== plugin [core] lines ==" -ForegroundColor Cyan
foreach ($name in 'SkyrimXER.log', 'skyrimxer_er.log') {
    $file = Join-Path $dest $name
    if (Test-Path -LiteralPath $file) { Select-String -LiteralPath $file -Pattern '\[core\]|\[error\]|\[critical\]' | ForEach-Object { $_.Line } }
}
Write-Host "`nlogs saved to $dest" -ForegroundColor Green
