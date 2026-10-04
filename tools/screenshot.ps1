<#
.SYNOPSIS
  Screenshots of the screen for the agent to look at: one frame, or a contact sheet of -Frames shots (-IntervalMs apart, after -LeadMs).
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/screenshot.ps1                                   # logs/shots/<time>.png
  powershell -ExecutionPolicy Bypass -File tools/screenshot.ps1 -Frames 8 -IntervalMs 110 -LeadMs 600 -Crop
.NOTES
  Pair it with a key script for a roll: start this in the background, then tools/game-input.ps1 -Keys "down W; wait 300; tap LShift 60; ...".
  -Crop keeps the middle of the screen (where the third-person character stands). Output goes to logs/shots/ (gitignored: game
  imagery never goes into git).
#>
[CmdletBinding()]
param(
    [string]$Out = '',
    [int]$Frames = 1,
    [int]$IntervalMs = 110,
    [int]$LeadMs = 0,
    [switch]$Crop
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms

if (-not $Out) {
    $dir = Join-Path (Split-Path -Parent $PSScriptRoot) 'logs\shots'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $Out = Join-Path $dir ((Get-Date -Format 'yyyyMMdd-HHmmss') + '.png')
}
if ($LeadMs -gt 0) { Start-Sleep -Milliseconds $LeadMs }

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
# Middle ~52% x 86% of the screen when cropping; tiles are scaled so the sheet stays small enough to read as one image.
$src = if ($Crop) {
    New-Object System.Drawing.Rectangle ([int]($bounds.Width * 0.24)), ([int]($bounds.Height * 0.14)), ([int]($bounds.Width * 0.52)), ([int]($bounds.Height * 0.86))
} else { New-Object System.Drawing.Rectangle 0, 0, $bounds.Width, $bounds.Height }
$tileW = if ($Frames -gt 1) { 300 } else { 960 }
$tileH = [int]($tileW * $src.Height / $src.Width)
$tiles = @()
for ($i = 0; $i -lt $Frames; $i++) {
    $full = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $g = [System.Drawing.Graphics]::FromImage($full)
    $g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $tiles += New-Object System.Drawing.Bitmap ($full.Clone($src, $full.PixelFormat)), $tileW, $tileH
    $g.Dispose(); $full.Dispose()
    if ($i -lt $Frames - 1) { Start-Sleep -Milliseconds $IntervalMs }
}
$cols = [math]::Min(4, $Frames)
$rows = [math]::Ceiling($Frames / $cols)
$sheet = New-Object System.Drawing.Bitmap ($tileW * $cols), ($tileH * $rows)
$gs = [System.Drawing.Graphics]::FromImage($sheet)
for ($i = 0; $i -lt $Frames; $i++) { $gs.DrawImage($tiles[$i], $tileW * ($i % $cols), $tileH * [math]::Floor($i / $cols)) }
$sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "screenshot: $Out"
