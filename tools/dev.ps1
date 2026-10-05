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
  Loading in is automatic (tools/game-input.psm1): ER gets the confirm key at the title screen until its player spawns; Skyrim loads its
   most recent save by itself (launch.ps1 -AutoLoad) and gets the focus at the end (it pauses unfocused). -Manual = the user does both.
  -SkyrimKeys "<script>": once Skyrim is in the world, run a key script on it (game-input.ps1 syntax) and print Skyrim's log lines.
  -SkyrimPad "<script>": the same with the dev virtual pad (tools/pad-input.ps1 syntax: tap Touchpad; wait 1500; down R2; ...).
  -WaitInWorld N (ER): once the ER player is in the world, let ER run N more seconds,
   then print the ER plugin's log lines whose subsystem matches -Show. Tests then need no "done" message from the user.
  -Restart stops the game(s) about to be launched first (ER is hidden in-world, so it gets Stop-Process; no clean Bye).
  -ErVisible / -ErSelfTest / -ErInjectGroup / -ErProbe / -ErDump / -ErForceCombat / -ErNoPin / -ErPoseProbe / -ErStance / -ErWeapon are written to build/er-plugin/skyrimxer_er.cfg on every launch (er-plugin/src/config.rs).
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
    [ValidateSet('', 'dodge', 'roll', 'walk', 'sprint')]
    [string]$ErSelfTest = '',
    [ValidateSet('', 'wprep', 'padstep', 'ailogic', 'prebehavior')]
    [string]$ErInjectGroup = '',
    [switch]$ErProbe,
    [switch]$ErDump,
    [ValidateSet('', 'on', 'off')]
    [string]$ErForceCombat = '',
    [switch]$ErNoPin,
    [switch]$ErPoseProbe,
    [ValidateSet('', 'empty', 'one', 'right2', 'left2', 'fists')]
    [string]$ErStance = '',
    [int]$ErWeapon = 0,
    [int]$WaitInWorld = 0,
    [switch]$Manual,
    [string]$SkyrimKeys = '',
    [string]$SkyrimPad = '',
    [string]$Show = 'action|state|window|probe|error|warning'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'game-input.psm1') -Force
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
    $cfg = @('# written by tools/dev.ps1 on every launch', "visible=$([int][bool]$ErVisible)", "selftest=$ErSelfTest", "inject_group=$ErInjectGroup", "probe=$([int][bool]$ErProbe)", "dump=$([int][bool]$ErDump)", "force_combat=$ErForceCombat", "pin=$([int](-not $ErNoPin))", "pose_probe=$([int][bool]$ErPoseProbe)", "stance=$ErStance", "weapon=$ErWeapon")
    Set-Content -Path (Join-Path $paths.BuildDir 'er-plugin\skyrimxer_er.cfg') -Value $cfg -Encoding ascii
}

if ($Game -in 'both', 'eldenring') {
    # A just-killed ER may have written its log seconds ago: its old "ready"/"in world" lines must not count for this launch.
    try { Remove-Item -LiteralPath (Join-Path $paths.BuildDir 'er-plugin\logs\skyrimxer_er.log') -ErrorAction Stop } catch {}
}
$launchedAt = Get-Date
Step "launch ($Game)"
& (Join-Path $PSScriptRoot 'launch.ps1') -Game $Game -AutoLoad:(-not $Manual)

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
$skyLog = Join-Path $paths.SkyrimLogDir 'SkyrimXER.log'
$erInWorld = { Select-String -LiteralPath $erLog -Pattern 'main player spawned \(in world' -Quiet }

# ER: press the menu-confirm key at the title screen ("press any button", then Continue is preselected) until the player spawns.
# E and Enter both confirm with default binds; alternating covers a rebound one. The window must be in front for the key to count.
if (-not $Manual -and $Game -in 'both', 'eldenring' -and $pending.Count -eq 0) {
    Step 'Elden Ring: loading in (confirm key at the title screen)'
    $deadline = (Get-Date).AddSeconds(90)
    $presses = 0
    $lastKey = ''
    while (-not (& $erInWorld) -and (Get-Date) -lt $deadline) {
        $key = @('E', 'E', 'Enter', 'Enter')[$presses % 4]
        if (Set-GameFocus eldenring -TimeoutSeconds 5) {
            try { Send-GameKeys -Game eldenring -Script "tap $key 80" -NoFocus; $lastKey = $key; $presses++ } catch { Write-Host "  $_" -ForegroundColor Yellow }
        } else {
            Write-Host '  could not bring Elden Ring to the front' -ForegroundColor Yellow
        }
        for ($i = 0; $i -lt 6 -and -not (& $erInWorld); $i++) { Start-Sleep -Milliseconds 500 }
    }
    if (& $erInWorld) { Write-Host "  ER in world after $presses key press(es), last key $lastKey" -ForegroundColor Green }
    else { Write-Host 'ER did not load in within 90 s (title screen not reached, or the confirm key is rebound)' -ForegroundColor Yellow }
}

# Skyrim: the plugin loads the most recent save at the main menu (AutoLoad.cpp); then give it the focus, or it pauses.
$skyInWorld = $false
if (-not $Manual -and $Game -in 'both', 'skyrim' -and $pending.Count -eq 0) {
    Step 'Skyrim: waiting for the auto-loaded save'
    $deadline = (Get-Date).AddSeconds(120)
    while (-not (Select-String -LiteralPath $skyLog -Pattern 'save loaded \(success=true\)' -Quiet) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
    $skyInWorld = [bool](Select-String -LiteralPath $skyLog -Pattern 'save loaded \(success=true\)' -Quiet)
    if (-not $skyInWorld) { Write-Host 'Skyrim did not load a save within 120 s (see [autoload] lines in SkyrimXER.log)' -ForegroundColor Yellow }
    if (Set-GameFocus skyrim) { Write-Host '  Skyrim in world and focused' -ForegroundColor Green }
    else { Write-Host '  could not bring Skyrim to the front: click its window once (it pauses unfocused)' -ForegroundColor Yellow }
}

if ($SkyrimPad -and $skyInWorld) {
    Step 'Skyrim: virtual pad script'
    Start-Sleep -Seconds 3  # the world fades in after the load message
    $padFrom = Get-Date
    & (Join-Path $PSScriptRoot 'pad-input.ps1') -Script $SkyrimPad -Wait
    Step 'Skyrim log lines during the pad script'
    Get-Content -LiteralPath $skyLog | Where-Object { $_ -match '\[(input|pad|padscript)\]' } |
        Where-Object { [datetime]::Parse(($_ -split ' ')[0]).ToLocalTime() -ge $padFrom.AddSeconds(-1) } | Select-Object -Last 60
}

if ($SkyrimKeys -and $skyInWorld) {
    Step 'Skyrim: key script'
    Start-Sleep -Seconds 3  # the world fades in after the load message
    $keysFrom = Get-Date
    Send-GameKeys -Game skyrim -Script $SkyrimKeys
    Start-Sleep -Seconds 1
    Step 'Skyrim log lines during the key script'
    Get-Content -LiteralPath $skyLog | Where-Object { $_ -match '\[(input|move|state|pose|autoload)\]' } |
        Where-Object { [datetime]::Parse(($_ -split ' ')[0]).ToLocalTime() -ge $keysFrom.AddSeconds(-1) } | Select-Object -Last 60
}

if ($WaitInWorld -gt 0 -and $Game -in 'both', 'eldenring' -and $pending.Count -eq 0) {
    Step "waiting for the player to load in$(if ($Manual) { ' (user: press Continue in Elden Ring)' })"
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
