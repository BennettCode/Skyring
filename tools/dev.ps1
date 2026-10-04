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
  -WaitInWorld N (ER): after the plugins report in, wait for the user to press Continue in ER (up to 5 min), let ER run N more seconds,
   then print the ER plugin's log lines whose subsystem matches -Show. Tests then need no "done" message from the user.
  -Restart stops the game(s) about to be launched first (ER is hidden in-world, so it gets Stop-Process; no clean Bye).
  -ErVisible / -ErSelfTest / -ErInjectGroup / -ErProbe / -ErDump / -ErForceCombat are written to build/er-plugin/skyrimxer_er.cfg on every launch (er-plugin/src/config.rs).
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
    [ValidateSet('', 'dodge', 'roll')]
    [string]$ErSelfTest = '',
    [ValidateSet('', 'wprep', 'padstep', 'ailogic', 'prebehavior')]
    [string]$ErInjectGroup = '',
    [switch]$ErProbe,
    [switch]$ErDump,
    [ValidateSet('', 'on', 'off')]
    [string]$ErForceCombat = '',
    [int]$WaitInWorld = 0,
    [string]$Show = 'action|state|window|probe|error|warning'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force
$paths = Get-ProjectPaths
$timer = [Diagnostics.Stopwatch]::StartNew()

function Step([string]$Name) { Write-Host ("`n[{0,4}s] == {1} ==" -f [int]$timer.Elapsed.TotalSeconds, $Name) -ForegroundColor Cyan }

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
if ($Game -in 'both', 'eldenring' -and -not (Get-Process -Name steam -ErrorAction SilentlyContinue)) {
    # me3 fails with "Steam is required to run this game" (logs/me3-*.log) and we'd wait for the timeout.
    throw 'Steam is not running. Start Steam (offline mode is fine) and rerun.'
}

if ($Game -in 'both', 'eldenring') {
    $cfg = @('# written by tools/dev.ps1 on every launch', "visible=$([int][bool]$ErVisible)", "selftest=$ErSelfTest", "inject_group=$ErInjectGroup", "probe=$([int][bool]$ErProbe)", "dump=$([int][bool]$ErDump)", "force_combat=$ErForceCombat")
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

$erLog = Join-Path $paths.BuildDir 'er-plugin\logs\skyrimxer_er.log'
if ($WaitInWorld -gt 0 -and $Game -in 'both', 'eldenring' -and $pending.Count -eq 0) {
    Step 'waiting for the player to load in (user: press Continue in Elden Ring)'
    $inWorldDeadline = (Get-Date).AddMinutes(5)
    while (-not (Select-String -LiteralPath $erLog -Pattern 'main player spawned \(in world' -Quiet) -and (Get-Date) -lt $inWorldDeadline) {
        Start-Sleep -Seconds 2
    }
    if (Select-String -LiteralPath $erLog -Pattern 'main player spawned \(in world' -Quiet) {
        Step "in world; letting ER run ${WaitInWorld}s"
        Start-Sleep -Seconds $WaitInWorld
        Step "ER log [$Show] lines"
        $lines = @(Get-Content -LiteralPath $erLog | Where-Object { $_ -match "\[($Show)\]" })
        # Probe lines come in bursts; keep the first 40 so the summary stays short (the full log is collected below).
        $probe = @($lines | Where-Object { $_ -match '\[probe\]' } | Select-Object -First 40)
        $lines | Where-Object { $_ -notmatch '\[probe\]' } | Select-Object -Last 60 | ForEach-Object { $_ -replace '^\S+ \[ER\] ', '' }
        if ($probe.Count) { '-- first probe lines --'; $probe | ForEach-Object { $_ -replace '^\S+ \[ER\] \[info\] \[probe\] ', '' } }
    } else {
        Write-Host 'player never loaded in (nobody pressed Continue within 5 min)' -ForegroundColor Yellow
    }
}

Step 'collect logs'
& (Join-Path $PSScriptRoot 'collect-logs.ps1')

if ($pending.Count -gt 0) {
    Write-Host "NOT ready after ${WaitSeconds}s: $(($pending | ForEach-Object { $_.Name }) -join ', ')" -ForegroundColor Red
    exit 1
}
Step "all plugins reported in after $([int]$timer.Elapsed.TotalSeconds)s"
