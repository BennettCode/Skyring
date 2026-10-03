<#
.SYNOPSIS
  Verifies the dev environment for Skyrim X Elden Ring. Read-only: changes nothing.
.DESCRIPTION
  Checks the pinned game versions, SKSE, Address Library, Crash Logger, me3, the VS C++ toolchain
  (CMake + vcpkg bundled with VS), Rust and git. Exit code 0 = all required checks pass.
  Paths come from local/paths.json (template: config/paths.example.json).
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot

# Pinned versions (update together with docs/RECON.md and the eldenring-rs rev in Cargo.toml).
$Expected = @{
    SkyrimExe      = '1.7.104.0'
    SkseRuntimeDll = 'skse64_1_7_104.dll'
    SkseVersion    = '0, 2, 3, 1'
    AddressLibBin  = 'versionlib-1-7-104-0.bin'
    EldenRingExe   = '2.7.1.0'
}

$script:Failures = 0
$script:Warnings = 0

function Write-Check {
    param([string]$Status, [string]$Name, [string]$Detail, [string]$Fix = '')
    $line = '[{0,-4}] {1,-28} {2}' -f $Status, $Name, $Detail
    switch ($Status) {
        'OK'   { Write-Host $line -ForegroundColor Green }
        'WARN' { Write-Host $line -ForegroundColor Yellow; $script:Warnings++ }
        'FAIL' { Write-Host $line -ForegroundColor Red; $script:Failures++ }
    }
    if ($Fix -and $Status -ne 'OK') { Write-Host "         fix: $Fix" -ForegroundColor DarkGray }
}

function Expand-ConfigPath([string]$Path) {
    [Environment]::ExpandEnvironmentVariables($Path) -replace '/', '\'
}

function Get-FileVersionString([string]$Path) {
    (Get-Item -LiteralPath $Path).VersionInfo.FileVersion
}

$pathsFile = Join-Path $RepoRoot 'local\paths.json'
if (-not (Test-Path -LiteralPath $pathsFile)) {
    Write-Check 'FAIL' 'local/paths.json' 'missing' 'copy config/paths.example.json to local/paths.json and edit it'
    exit 1
}
$paths = Get-Content -LiteralPath $pathsFile -Raw | ConvertFrom-Json

Write-Host "`n== Skyrim (host) ==" -ForegroundColor Cyan
$sky = Expand-ConfigPath $paths.skyrim.install
$skyExe = Join-Path $sky $paths.skyrim.exe
if (Test-Path -LiteralPath $skyExe) {
    $v = Get-FileVersionString $skyExe
    if ($v -eq $Expected.SkyrimExe) { Write-Check 'OK' 'SkyrimSE.exe' $v }
    else { Write-Check 'FAIL' 'SkyrimSE.exe' "$v (expected $($Expected.SkyrimExe))" 'the game updated or downgraded: update SKSE/Address Library/CommonLib pins before continuing' }
} else { Write-Check 'FAIL' 'SkyrimSE.exe' "not found at $skyExe" 'fix skyrim.install in local/paths.json' }

$skseDll = Join-Path $sky $Expected.SkseRuntimeDll
if (Test-Path -LiteralPath $skseDll) {
    $v = Get-FileVersionString $skseDll
    if ($v -eq $Expected.SkseVersion) { Write-Check 'OK' 'SKSE runtime' "$($Expected.SkseRuntimeDll) ($v)" }
    else { Write-Check 'WARN' 'SKSE runtime' "$($Expected.SkseRuntimeDll) version $v (expected $($Expected.SkseVersion))" }
} else { Write-Check 'FAIL' 'SKSE runtime' "$($Expected.SkseRuntimeDll) missing" 'install SKSE 2.3.1 (Nexus mod 30379)' }

$plugins = Join-Path $sky (Expand-ConfigPath $paths.skyrim.pluginsDir)
if (Test-Path -LiteralPath (Join-Path $plugins $Expected.AddressLibBin)) { Write-Check 'OK' 'Address Library' $Expected.AddressLibBin }
else { Write-Check 'FAIL' 'Address Library' "$($Expected.AddressLibBin) missing" 'install Address Library All in One (Nexus mod 32444)' }

if (Test-Path -LiteralPath (Join-Path $plugins 'CrashLogger.dll')) { Write-Check 'OK' 'Crash Logger' 'CrashLogger.dll present' }
else { Write-Check 'WARN' 'Crash Logger' 'not installed' 'install from github.com/alandtse/CrashLoggerSSE releases' }

$skseLog = Join-Path (Expand-ConfigPath $paths.skyrim.logDir) 'skse64.log'
if (Test-Path -LiteralPath $skseLog) {
    $first = Get-Content -LiteralPath $skseLog -TotalCount 1
    if ($first -match 'version = 2\.3\.1') { Write-Check 'OK' 'skse64.log' "last run used SKSE 2.3.1 ($((Get-Item -LiteralPath $skseLog).LastWriteTime))" }
    else { Write-Check 'WARN' 'skse64.log' "last run: $first" 'launch Skyrim once via skse64_loader.exe' }
} else { Write-Check 'WARN' 'skse64.log' 'not found (SKSE never ran?)' 'launch Skyrim once via skse64_loader.exe' }

Write-Host "`n== Elden Ring (hidden game) ==" -ForegroundColor Cyan
$er = Expand-ConfigPath $paths.eldenRing.install
$erExe = Join-Path $er $paths.eldenRing.exe
if (Test-Path -LiteralPath $erExe) {
    $v = Get-FileVersionString $erExe
    if ($v -eq $Expected.EldenRingExe) { Write-Check 'OK' 'eldenring.exe' "$v (patch 1.17.1.0)" }
    else { Write-Check 'FAIL' 'eldenring.exe' "$v (expected $($Expected.EldenRingExe))" 'the game updated: eldenring-rs only supports pinned versions, check for an update' }
} else { Write-Check 'FAIL' 'eldenring.exe' "not found at $erExe" 'fix eldenRing.install in local/paths.json' }

# Shells started before the me3 install have a stale PATH, so fall back to the installer's default location.
$me3Path = $null
$me3 = Get-Command me3 -ErrorAction SilentlyContinue
if ($me3) { $me3Path = $me3.Source }
else {
    $default = Join-Path $env:LOCALAPPDATA 'Programs\garyttierney\me3\bin\me3.exe'
    if (Test-Path -LiteralPath $default) { $me3Path = $default }
}
if ($me3Path) {
    $v = (& $me3Path --version) -join ' '
    Write-Check 'OK' 'me3' "$v ($me3Path)"
} else { Write-Check 'FAIL' 'me3' 'not installed' 'run the me3 installer from github.com/garyttierney/me3 releases' }

$erSaves = Expand-ConfigPath $paths.eldenRing.savesDir
if (Test-Path -LiteralPath (Join-Path $erSaves 'skyrimxer.sl2')) { Write-Check 'OK' 'ER dev save' 'skyrimxer.sl2 exists' }
else { Write-Check 'WARN' 'ER dev save' 'skyrimxer.sl2 not created yet' 'launch once: me3 launch -g eldenring --savefile skyrimxer.sl2' }

Write-Host "`n== Toolchain ==" -ForegroundColor Cyan
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ($vsPath) {
    Write-Check 'OK' 'Visual Studio C++' $vsPath
    $devShell = Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1'
    if (Test-Path -LiteralPath $devShell) { Write-Check 'OK' 'VS Developer Shell' 'Launch-VsDevShell.ps1 present' }
    else { Write-Check 'FAIL' 'VS Developer Shell' 'Launch-VsDevShell.ps1 missing' }
    $cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $cmake) { Write-Check 'OK' 'CMake (VS bundled)' ((& $cmake --version | Select-Object -First 1)) }
    else { Write-Check 'FAIL' 'CMake (VS bundled)' 'missing' 'add "C++ CMake tools for Windows" in the VS installer' }
    if (Test-Path -LiteralPath (Join-Path $vsPath 'VC\vcpkg\.vcpkg-root')) { Write-Check 'OK' 'vcpkg (VS bundled)' 'VC\vcpkg present' }
    else { Write-Check 'FAIL' 'vcpkg (VS bundled)' 'missing' 'add "vcpkg package manager" in the VS installer' }
} else { Write-Check 'FAIL' 'Visual Studio C++' 'no VS with x64 C++ tools found' 'install VS 2026 with "Desktop development with C++"' }

$cargo = Get-Command cargo -ErrorAction SilentlyContinue
if ($cargo) { Write-Check 'OK' 'Rust (cargo)' ((& $cargo.Source --version) -join ' ') }
else { Write-Check 'FAIL' 'Rust (cargo)' 'not on PATH' 'install rustup (stable-x86_64-pc-windows-msvc)' }

$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) { Write-Check 'OK' 'git' ((& $git.Source --version) -join ' ') }
else { Write-Check 'FAIL' 'git' 'not on PATH' }

Write-Host "`n== Safety ==" -ForegroundColor Cyan
$running = Get-Process -Name 'start_protected_game', 'EasyAntiCheat*' -ErrorAction SilentlyContinue
if ($running) { Write-Check 'WARN' 'EAC processes' (($running | ForEach-Object Name) -join ', ') 'close Elden Ring and relaunch through me3' }
else { Write-Check 'OK' 'EAC processes' 'none running' }

Write-Host ''
if ($script:Failures -gt 0) {
    Write-Host "$($script:Failures) check(s) FAILED, $($script:Warnings) warning(s)." -ForegroundColor Red
    exit 1
}
Write-Host "All required checks passed ($($script:Warnings) warning(s))." -ForegroundColor Green
exit 0
