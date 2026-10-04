<#
.SYNOPSIS
  Focuses a game window and/or sends a key script to it (tools/game-input.psm1).
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/game-input.ps1 -Game skyrim -Focus
  powershell -ExecutionPolicy Bypass -File tools/game-input.ps1 -Game skyrim -Keys "down W; wait 500; tap LShift 60; wait 1500; up W"
.NOTES
  Steps: down K | up K | tap K [ms] | wait ms. Keys: W A S D LShift Space Enter E Esc Tab Q R F F1 F5 F7-F10 1-4 Up/Down/Left/Right ...
  The script stops and releases every key if the game stops being the foreground window.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('skyrim', 'eldenring')][string]$Game,
    [switch]$Focus,
    [string]$Keys = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'game-input.psm1') -Force

if ($Focus -or -not $Keys) {
    if (-not (Set-GameFocus $Game)) { throw "could not bring $Game to the front" }
    Write-Host "$Game focused" -ForegroundColor Green
}
if ($Keys) { Send-GameKeys -Game $Game -Script $Keys }
