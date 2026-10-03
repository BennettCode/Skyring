<#
.SYNOPSIS
  Builds the Skyrim plugin (skse/) and/or the Elden Ring plugin (er-plugin/) into build/.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/build.ps1            # both
  powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Target er # Elden Ring plugin only
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'skse', 'er')]
    [string]$Target = 'all'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force

$paths = Get-ProjectPaths
New-Item -ItemType Directory -Force $paths.LogsDir | Out-Null

function Build-Skse {
    Write-Host '== Building SkyrimXER.dll (skse/) ==' -ForegroundColor Cyan
    $devShell = Get-VsDevShellScript
    $skseDir = Join-Path $paths.Repo 'skse'
    $log = Join-Path $paths.LogsDir 'skse-build.log'
    # The VS Developer Shell rewrites the environment, so run it in a child process.
    $cmd = "& '$devShell' -Arch amd64 -SkipAutomaticLocation | Out-Null; Set-Location '$skseDir'; " +
        "cmake --preset default; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; cmake --build --preset default; exit `$LASTEXITCODE"
    # PS 5.1 turns a native command's stderr into error records; with 'Stop' that would abort on the dev shell's
    # harmless "vswhere.exe is not recognized" notice. Judge success by the exit code instead.
    $ErrorActionPreference = 'Continue'
    powershell -NoProfile -ExecutionPolicy Bypass -Command $cmd *> $log
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Select-String -LiteralPath $log -Pattern 'error C\d+|error LNK|CMake Error|warning C\d+' | Select-Object -First 20 | ForEach-Object { Write-Host $_.Line }
    if ($code -ne 0) { throw "SKSE build failed (exit $code). Full log: $log" }
    $dll = Join-Path $paths.BuildDir 'skse\SkyrimXER.dll'
    if (-not (Test-Path -LiteralPath $dll)) { throw "Build reported success but $dll is missing." }
    Write-Host "OK: $dll" -ForegroundColor Green
}

function Build-Er {
    Write-Host '== Building skyrimxer_er.dll (er-plugin/) ==' -ForegroundColor Cyan
    Push-Location $paths.Repo
    try {
        cargo build --release -p skyrimxer-er
        if ($LASTEXITCODE -ne 0) { throw "cargo build failed (exit $LASTEXITCODE)." }
    } finally { Pop-Location }
    $out = Join-Path $paths.BuildDir 'er-plugin'
    New-Item -ItemType Directory -Force $out | Out-Null
    foreach ($name in 'skyrimxer_er.dll', 'skyrimxer_er.pdb') {
        $src = Join-Path $paths.Repo "target\release\$name"
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $out -Force }
    }
    Write-Host "OK: $(Join-Path $out 'skyrimxer_er.dll')" -ForegroundColor Green
}

if ($Target -in 'all', 'skse') { Build-Skse }
if ($Target -in 'all', 'er') { Build-Er }
