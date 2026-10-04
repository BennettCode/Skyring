<#
.SYNOPSIS
  Copies every relevant log from both games into logs/<timestamp>/ and prints the [core]/[link]/error lines from both plugins
  (periodic "heartbeat seq=" lines are summarised: count + the last one).
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

Write-Host "`n== plugin [core]/[link] lines ==" -ForegroundColor Cyan
foreach ($name in 'SkyrimXER.log', 'skyrimxer_er.log') {
    $file = Join-Path $dest $name
    if (-not (Test-Path -LiteralPath $file)) { continue }
    $lines = @(Select-String -LiteralPath $file -Pattern '\[core\]|\[link\]|\[error\]|\[critical\]|\[warn(ing)?\]' | ForEach-Object { $_.Line })
    # Heartbeat events arrive every 5 s; show how many plus the last one instead of all of them.
    $beats = @($lines | Where-Object { $_ -match 'heartbeat seq=' })
    $lines | Where-Object { $_ -notmatch 'heartbeat seq=' }
    if ($beats.Count) { Write-Host "  ($name`: $($beats.Count) heartbeat events received, last one:)" -ForegroundColor DarkGray; $beats[-1] }
}
Write-Host "`nlogs saved to $dest" -ForegroundColor Green
