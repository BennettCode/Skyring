<#
.SYNOPSIS
  Checks Address Library IDs against the installed database before a plugin uses them.
.DESCRIPTION
  CommonLib aborts the game at load when an ID is missing from versionlib-<SkyrimSE.exe version>.bin, so run this for every
  new REL::ID / RELOCATION_ID (AE id) before building it in. Reads the database from the Skyrim install in local/paths.json.
  Supports the format-5 database (dense u32 offset table after a 96-byte header; a missing id reads as 0), which is what
  Address Library v13 ships for 1.7.104. Exit code 1 if any id is missing.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/addrlib-check.ps1 -Id 208040,402776
#>
[CmdletBinding()]
param(
    # Comma-separated or repeated; plain decimal ids. (powershell -File passes a comma list as one string.)
    [Parameter(Mandatory)][string[]]$Id
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force

$paths = Get-ProjectPaths
$exe = Join-Path $paths.SkyrimDir 'SkyrimSE.exe'
$v = (Get-Item -LiteralPath $exe).VersionInfo
$version = "$($v.FileMajorPart)-$($v.FileMinorPart)-$($v.FileBuildPart)-$($v.FilePrivatePart)"
$db = Join-Path $paths.SksePlugins "versionlib-$version.bin"
if (-not (Test-Path -LiteralPath $db)) { throw "$db not found (Address Library for SkyrimSE.exe $version is not installed)" }

$bytes = [System.IO.File]::ReadAllBytes($db)
$format = [BitConverter]::ToInt32($bytes, 0)
if ($format -ne 5) { throw "$db is format $format; this script only reads format 5" }
$count = [BitConverter]::ToInt32($bytes, 92)
if ($bytes.Length -lt 96 + 4 * $count) { throw "$db is truncated" }
Write-Host "Address Library $db (format 5, $count ids)"

$ids = $Id | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() } | ForEach-Object { [uint32]$_.Trim() }
$missing = 0
foreach ($i in $ids) {
    $offset = if ($i -lt $count) { [BitConverter]::ToUInt32($bytes, 96 + 4 * $i) } else { 0 }
    if ($offset -eq 0) {
        Write-Host ("  {0,8}  MISSING" -f $i) -ForegroundColor Red
        $missing++
    } else {
        Write-Host ("  {0,8}  0x{1:x}" -f $i, $offset)
    }
}
if ($missing) { Write-Host "$missing id(s) missing: the game would abort at load" -ForegroundColor Red; exit 1 }
exit 0
