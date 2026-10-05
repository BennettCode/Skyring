<#
.SYNOPSIS
  Backs up saves, then launches Elden Ring (through me3, offline, dev save) and/or Skyrim (through SKSE).
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/launch.ps1 -Game eldenring
  powershell -ExecutionPolicy Bypass -File tools/launch.ps1 -Game skyrim
  powershell -ExecutionPolicy Bypass -File tools/launch.ps1            # both: ER first, then Skyrim
.NOTES
  Skyrim starts through skse64_loader.exe directly (default). Skyrim then gets no Steam Input, so no DualSense: keyboard/mouse only
  for now. -SkyrimVia steam starts it with steam://rungameid/489830 instead (needs a Steam launch option that runs SKSE; the plain
  `"...\skse64_loader.exe" %command%` form fails: the loader rejects the appended path, "too many free args"). Controller: later.
  -AutoLoad (dev, loader only): sets SKYRIMXER_AUTOLOAD=<newest save name> for Skyrim, so the plugin loads it when the main menu
  opens (skse/src/bridge/AutoLoad.cpp). Without it nothing changes.
#>
[CmdletBinding()]
param(
    [ValidateSet('both', 'eldenring', 'skyrim')]
    [string]$Game = 'both',
    [switch]$SkipBackup,
    [ValidateSet('steam', 'loader')]
    [string]$SkyrimVia = 'loader',
    [switch]$AutoLoad
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force

$paths = Get-ProjectPaths
Assert-NoEasyAntiCheat
if (-not $SkipBackup) { & (Join-Path $PSScriptRoot 'backup-saves.ps1') }
New-Item -ItemType Directory -Force $paths.LogsDir | Out-Null

if ($Game -in 'both', 'eldenring') {
    $dll = Join-Path $paths.BuildDir 'er-plugin\skyrimxer_er.dll'
    if (-not (Test-Path -LiteralPath $dll)) { throw "$dll not found. Run tools/build.ps1 -Target er first." }
    $me3 = Get-Me3Path
    $stamp = Get-Timestamp
    # --savefile keeps the user's real ER0000.sl2 untouched. Never add --online.
    $me3Args = @('launch', '-g', 'eldenring', '-p', "`"$($paths.Me3Profile)`"", '--savefile', 'skyrimxer.sl2')
    Start-Process -FilePath $me3 -ArgumentList $me3Args -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $paths.LogsDir "me3-$stamp-stdout.log") `
        -RedirectStandardError (Join-Path $paths.LogsDir "me3-$stamp.log")
    Write-Host "Elden Ring launching through me3 (log: logs/me3-$stamp.log)" -ForegroundColor Green
}

if ($Game -in 'both', 'skyrim') {
    if (-not (Test-Path -LiteralPath $paths.SkseLoader)) { throw "SKSE loader not found: $($paths.SkseLoader)" }
    if (-not (Test-Path -LiteralPath (Join-Path $paths.SksePlugins 'SkyrimXER.dll'))) {
        Write-Warning 'SkyrimXER.dll is not deployed; Skyrim will start without the mod. Run tools/deploy.ps1.'
    }
    if ($SkyrimVia -eq 'steam') {
        if (-not (Get-Process -Name steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running. Start Steam (offline mode is fine) and rerun.' }
        # Steam runs the launch option (skse64_loader.exe %command%), so SKSE loads AND Steam Input feeds the DualSense as XInput.
        Start-Process 'steam://rungameid/489830'
        Write-Host 'Skyrim launching through Steam (launch option -> skse64_loader). If SkyrimXER.log never appears, the launch option is missing.' -ForegroundColor Green
    } else {
        # skse64_loader passes its environment on to SkyrimSE.exe.
        # The newest save by name: at the moment the main menu opens, LoadMostRecentSaveGame still finds no saves (2026-10-04).
        $newest = Get-ChildItem -LiteralPath $paths.SkyrimSaves -Filter *.ess -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($AutoLoad -and -not $newest) { Write-Warning "no Skyrim saves in $($paths.SkyrimSaves); load one by hand" }
        if ($AutoLoad -and $newest) { $env:SKYRIMXER_AUTOLOAD = $newest.BaseName } else { Remove-Item Env:SKYRIMXER_AUTOLOAD -ErrorAction SilentlyContinue }
        # Dev virtual pad (bridge/PadScript.cpp): tools/pad-input.ps1 writes this file, the plugin plays it. Dev launches only.
        if ($AutoLoad) { $env:SKYRIMXER_PADSCRIPT = Join-Path (Split-Path $PSScriptRoot -Parent) 'local\padscript.txt' }
        Start-Process -FilePath $paths.SkseLoader -WorkingDirectory $paths.SkyrimDir
        Remove-Item Env:SKYRIMXER_AUTOLOAD -ErrorAction SilentlyContinue
        Remove-Item Env:SKYRIMXER_PADSCRIPT -ErrorAction SilentlyContinue
        if ($AutoLoad -and $newest) { Write-Host "Skyrim will load the newest save by itself ($($newest.Name))" -ForegroundColor Green }
        Write-Host 'Skyrim launching through skse64_loader.exe (no Steam Input: keyboard/mouse only)' -ForegroundColor Green
    }
}
