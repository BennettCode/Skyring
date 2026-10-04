<#
.SYNOPSIS
  Closes Skyrim and/or Elden Ring (clean close first, then kill). ER is hidden in-world, so it gets killed.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/stop-games.ps1 -Game eldenring
#>
[CmdletBinding()]
param(
    [ValidateSet('both', 'eldenring', 'skyrim')]
    [string]$Game = 'both'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'common.psm1') -Force
if ($Game -in 'both', 'skyrim') { Stop-Game 'SkyrimSE' }
if ($Game -in 'both', 'eldenring') { Stop-Game 'eldenring' }
