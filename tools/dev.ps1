<#
.SYNOPSIS
  One-command dev loop: build -> deploy -> back up saves -> launch -> wait for the plugins to report in -> collect logs.
.DESCRIPTION
  "Ready" means: Skyrim's log has kDataLoaded (main menu), and the ER log has its per-frame task registered (or an [error]).
  Exits non-zero if a build fails or a plugin doesn't report in before -WaitSeconds.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/dev.ps1                      # everything, both games
  powershell -ExecutionPolicy Bypass -File tools/dev.ps1 -Target er -Game eldenring
  powershell -ExecutionPolicy Bypass -File tools/dev.ps1 -NoLaunch            # build + deploy only
  powershell -ExecutionPolicy Bypass -File tools/dev.ps1 -Target er -Game eldenring -Restart -ErSelfTest dodge
.NOTES
  -Restart stops the game(s) about to be launched first (ER is hidden in-world, so it gets Stop-Process; no clean Bye).
  -ErVisible / -ErSelfTest / -ErInjectGroup / -ErProbe are written to build/er-plugin/skyrimxer_er.cfg on every launch (er-plugin/src/config.rs).
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'skse', 'er')]
    [string]$Target = 'all',
    [ValidateSet('both', 'eldenring', 'skyrim')]
    [string]$Game = 'both',
    [switch]$NoLaunch,
    [int]$WaitSeconds = 180,
    [switch]$Restart,
    [switch]$ErVisible,
    [ValidateSet('', 'dodge')]
    [string]$ErSelfTest = '',
    [ValidateSet('', 'wprep', 'padstep', 'ailogic', 'prebehavior')]
    [string]$ErInjectGroup = '',
    [switch]$ErProbe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force
$paths = Get-ProjectPaths
$timer = [Diagnostics.Stopwatch]::StartNew()

function Step([string]$Name) { Write-Host ("`n[{0,4}s] == {1} ==" -f [int]$timer.Elapsed.TotalSeconds, $Name) -ForegroundColor Cyan }

function Stop-Game([string]$Name) {
    $p = Get-Process -Name $Name -ErrorAction SilentlyContinue
    if (-not $p) { return }
    Write-Host "stopping $Name (pid $($p.Id -join ', '))"
    $p | ForEach-Object { if ($_.MainWindowHandle -ne 0) { [void]$_.CloseMainWindow() } }
    $p | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
    Get-Process -Name $Name -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process -Name $Name -ErrorAction SilentlyContinue | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}

if ($Restart -and -not $NoLaunch) {
    if ($Game -in 'both', 'skyrim') { Stop-Game 'SkyrimSE' }
    if ($Game -in 'both', 'eldenring') { Stop-Game 'eldenring' }
}

Step "build ($Target)"
& (Join-Path $PSScriptRoot 'build.ps1') -Target $Target

if ($Target -in 'all', 'skse') {
    if (Get-Process -Name SkyrimSE -ErrorAction SilentlyContinue) {
        throw 'Skyrim is running, so the new SkyrimXER.dll cannot be deployed. Quit Skyrim and rerun.'
    }
    Step 'deploy'
    & (Join-Path $PSScriptRoot 'deploy.ps1')
}

if ($NoLaunch) { Step "done (no launch) in $([int]$timer.Elapsed.TotalSeconds)s"; return }

if ($Game -in 'both', 'eldenring' -and (Get-Process -Name eldenring -ErrorAction SilentlyContinue)) {
    throw 'Elden Ring is already running (it would not load the new DLL). Quit it and rerun.'
}

if ($Game -in 'both', 'eldenring') {
    $cfg = @('# written by tools/dev.ps1 on every launch', "visible=$([int][bool]$ErVisible)", "selftest=$ErSelfTest", "inject_group=$ErInjectGroup", "probe=$([int][bool]$ErProbe)")
    Set-Content -Path (Join-Path $paths.BuildDir 'er-plugin\skyrimxer_er.cfg') -Value $cfg -Encoding ascii
}

$launchedAt = Get-Date
Step "launch ($Game)"
& (Join-Path $PSScriptRoot 'launch.ps1') -Game $Game

$checks = @()
if ($Game -in 'both', 'skyrim') {
    $checks += @{ Name = 'Skyrim'; File = (Join-Path $paths.SkyrimLogDir 'SkyrimXER.log'); Ready = 'kDataLoaded|\[critical\]' }
}
if ($Game -in 'both', 'eldenring') {
    $checks += @{ Name = 'Elden Ring'; File = (Join-Path $paths.BuildDir 'er-plugin\logs\skyrimxer_er.log'); Ready = 'per-frame task registered|\[error\]' }
}

Step "waiting up to ${WaitSeconds}s for: $(($checks | ForEach-Object { $_.Name }) -join ', ')"
$pending = [System.Collections.ArrayList]@($checks)
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ($pending.Count -gt 0 -and (Get-Date) -lt $deadline) {
    foreach ($c in @($pending)) {
        $f = $c.File
        if ((Test-Path -LiteralPath $f) -and (Get-Item -LiteralPath $f).LastWriteTime -ge $launchedAt.AddSeconds(-2) -and
            (Select-String -LiteralPath $f -Pattern $c.Ready -Quiet)) {
            Write-Host ("  ready: {0} ({1}s)" -f $c.Name, [int]$timer.Elapsed.TotalSeconds) -ForegroundColor Green
            [void]$pending.Remove($c)
        }
    }
    if ($pending.Count -gt 0) { Start-Sleep -Seconds 2 }
}

Step 'collect logs'
& (Join-Path $PSScriptRoot 'collect-logs.ps1')

if ($pending.Count -gt 0) {
    Write-Host "NOT ready after ${WaitSeconds}s: $(($pending | ForEach-Object { $_.Name }) -join ', ')" -ForegroundColor Red
    exit 1
}
Step "all plugins reported in after $([int]$timer.Elapsed.TotalSeconds)s"
