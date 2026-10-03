<#
.SYNOPSIS
  Copies SkyrimXER.dll/.pdb into Skyrim's Data\SKSE\Plugins and records them in local/deploy-manifest.json.
  -Undo removes exactly the files the manifest lists. The Elden Ring DLL is never deployed: me3 loads it from build/.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/deploy.ps1 -WhatIf
  powershell -ExecutionPolicy Bypass -File tools/deploy.ps1
  powershell -ExecutionPolicy Bypass -File tools/deploy.ps1 -Undo
#>
[CmdletBinding(SupportsShouldProcess)]
param([switch]$Undo)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force

$paths = Get-ProjectPaths
$manifestPath = Join-Path $paths.Repo 'local\deploy-manifest.json'

if (Get-Process -Name SkyrimSE -ErrorAction SilentlyContinue) {
    throw 'Skyrim is running; quit it first (the DLL is locked while the game runs).'
}

if ($Undo) {
    if (-not (Test-Path -LiteralPath $manifestPath)) { Write-Host 'Nothing to undo (no manifest).'; return }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach ($file in @($manifest.files)) {
        if ((Test-Path -LiteralPath $file) -and $PSCmdlet.ShouldProcess($file, 'Remove')) {
            Remove-Item -LiteralPath $file
            Write-Host "removed $file"
        }
    }
    if ($PSCmdlet.ShouldProcess($manifestPath, 'Remove manifest')) { Remove-Item -LiteralPath $manifestPath }
    return
}

$sources = @('SkyrimXER.dll', 'SkyrimXER.pdb') | ForEach-Object { Join-Path $paths.BuildDir "skse\$_" }
if (-not (Test-Path -LiteralPath $sources[0])) { throw "$($sources[0]) not found. Run tools/build.ps1 -Target skse first." }

$deployed = @()
foreach ($src in $sources) {
    if (-not (Test-Path -LiteralPath $src)) { continue }
    $dst = Join-Path $paths.SksePlugins (Split-Path -Leaf $src)
    if ($PSCmdlet.ShouldProcess($dst, "Copy from $src")) {
        New-Item -ItemType Directory -Force $paths.SksePlugins | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
        Write-Host "deployed $dst"
    }
    $deployed += $dst
}

if ($PSCmdlet.ShouldProcess($manifestPath, 'Write manifest')) {
    [ordered]@{ deployed = (Get-Date).ToString('s'); files = $deployed } | ConvertTo-Json | Out-File -LiteralPath $manifestPath -Encoding utf8
}
