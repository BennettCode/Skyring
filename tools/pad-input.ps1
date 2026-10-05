<#
.SYNOPSIS
  Plays a script on the dev virtual pad in Skyrim (skse/src/bridge/PadScript.*): the plugin watches local/padscript.txt and plays
  each new version once, on top of the real DualSense. Only works in Skyrim started by tools/launch.ps1 -AutoLoad / tools/dev.ps1.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/pad-input.ps1 -Script "tap Touchpad; wait 1500; tap Circle"
  powershell -ExecutionPolicy Bypass -File tools/pad-input.ps1 -Script "tap Create; wait 1200; tap R2; wait 800" -Wait
.NOTES
  Steps: down B | up B | tap B [ms, default 100] | wait ms | stick L|R x y (-1..1).
  B: Cross Circle Square Triangle L1 R1 L2 R2 Create Options L3 R3 PS Touchpad Up Down Left Right.
  -Wait returns after the script's length (sum of waits and taps) plus 300 ms. Skyrim must be focused (it pauses unfocused):
  tools/game-input.ps1 -Game skyrim -Focus.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Script,
    [switch]$Wait
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'game-input.psm1') -Force

if (-not (Set-GameFocus skyrim)) { throw 'could not bring Skyrim to the front' }
$file = Join-Path (Split-Path $PSScriptRoot -Parent) 'local\padscript.txt'
# A timestamp comment makes every write a new version, even for the same script.
Set-Content -LiteralPath $file -Value "# $(Get-Date -Format o); $Script" -Encoding ascii
Write-Host "pad script sent: $Script" -ForegroundColor Green
if ($Wait) {
    $ms = 0
    foreach ($step in ($Script -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        $parts = $step -split '\s+'
        if ($parts[0] -eq 'wait') { $ms += [int]$parts[1] }
        elseif ($parts[0] -eq 'tap') { $ms += $(if ($parts.Count -gt 2) { [int]$parts[2] } else { 100 }) }
    }
    Start-Sleep -Milliseconds ($ms + 500)
}
