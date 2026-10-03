# Shared helpers for tools/*.ps1. PowerShell 5.1-compatible.

Set-StrictMode -Version Latest

$script:RepoRoot = Split-Path -Parent $PSScriptRoot

function Get-RepoRoot { $script:RepoRoot }

function Expand-ConfigPath([string]$Path) {
    [Environment]::ExpandEnvironmentVariables($Path) -replace '/', '\'
}

# Resolved absolute paths from local/paths.json (template: config/paths.example.json).
function Get-ProjectPaths {
    $file = Join-Path $script:RepoRoot 'local\paths.json'
    if (-not (Test-Path -LiteralPath $file)) {
        throw "local/paths.json is missing. Copy config/paths.example.json to local/paths.json and edit it."
    }
    $p = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    $sky = Expand-ConfigPath $p.skyrim.install
    $er = Expand-ConfigPath $p.eldenRing.install
    [pscustomobject]@{
        Repo           = $script:RepoRoot
        SkyrimDir      = $sky
        SkseLoader     = Join-Path $sky $p.skyrim.skseLoader
        SksePlugins    = Join-Path $sky (Expand-ConfigPath $p.skyrim.pluginsDir)
        SkyrimSaves    = Expand-ConfigPath $p.skyrim.savesDir
        SkyrimLogDir   = Expand-ConfigPath $p.skyrim.logDir
        EldenRingDir   = $er
        EldenRingSaves = Expand-ConfigPath $p.eldenRing.savesDir
        Me3Profile     = Join-Path $script:RepoRoot (Expand-ConfigPath $p.eldenRing.me3Profile)
        BuildDir       = Join-Path $script:RepoRoot (Expand-ConfigPath $p.repo.buildDir)
        LogsDir        = Join-Path $script:RepoRoot (Expand-ConfigPath $p.repo.logsDir)
        SaveBackups    = Join-Path $script:RepoRoot (Expand-ConfigPath $p.repo.saveBackupsDir)
    }
}

# me3 may be missing from PATH in shells started before it was installed.
function Get-Me3Path {
    $cmd = Get-Command me3 -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $default = Join-Path $env:LOCALAPPDATA 'Programs\garyttierney\me3\bin\me3.exe'
    if (Test-Path -LiteralPath $default) { return $default }
    throw 'me3 not found. Install it from https://github.com/garyttierney/me3/releases'
}

function Get-VsDevShellScript {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with x64 C++ tools not found.' }
    Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1'
}

function Get-Timestamp { Get-Date -Format 'yyyyMMdd-HHmmss' }

function Assert-NoEasyAntiCheat {
    $eac = Get-Process -Name 'start_protected_game', 'EasyAntiCheat*' -ErrorAction SilentlyContinue
    if ($eac) {
        throw "Easy Anti-Cheat is running ($(($eac | ForEach-Object Name) -join ', ')). Close Elden Ring; modded ER must only run through me3."
    }
}

Export-ModuleMember -Function Get-RepoRoot, Expand-ConfigPath, Get-ProjectPaths, Get-Me3Path, Get-VsDevShellScript, Get-Timestamp, Assert-NoEasyAntiCheat
