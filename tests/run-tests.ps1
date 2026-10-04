<#
.SYNOPSIS
  All protocol/link tests, without launching either game. Must pass before committing a protocol or link change.
.DESCRIPTION
  1. cargo test (protogen: generated files up to date + validation; protocol crate: rings, link state machine).
  2. C++ selftest (skyrimxer_link_test.exe selftest; its build also compiles the generated static_asserts).
  3. Cross-language, real processes, private region name (safe while a game runs):
     A. Rust fake ER exits without Bye (crash)  -> C++ Skyrim link: handshake, heartbeat events, then timeout.
        Also the slots both ways: C++ Skyrim pulses Dodge (InputState), the Rust ER dodges (stamina -20, 1 s IFrame) and
        C++ Skyrim logs it from PlayerState, then marks PlayerState stale once after the crash. PoseState: the Rust ER's
        swinging pose is Active during each dodge and the C++ Skyrim sees it (24 bones, unit quaternions, pelvis swing).
     B. C++ ER link exits cleanly with Bye      -> Rust fake Skyrim: handshake, then "said Bye".
  Needs `tools/build.ps1 -Target skse` first (for the C++ exe). Exit code 0 = all passed.
#>
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cppExe = Join-Path $root 'build\skse\skyrimxer_link_test.exe'
$fakeExe = Join-Path $root 'target\debug\fake-peer.exe'
$outDir = Join-Path $root 'logs\tests'
New-Item -ItemType Directory -Force $outDir | Out-Null
$failures = 0

function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok    $what" -ForegroundColor Green } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failures++ }
}

function Invoke-Native([string]$label, [scriptblock]$block) {
    Write-Host "== $label ==" -ForegroundColor Cyan
    $ErrorActionPreference = 'Continue'  # cargo writes progress to stderr (PS 5.1 would treat it as an error)
    & $block 2>&1 | ForEach-Object { "$_" } | Select-String -Pattern 'test result|FAILED|panicked|error|selftest|ok  |FAIL' | ForEach-Object { "  $($_.Line.Trim())" }
    $code = $LASTEXITCODE
    Check ($code -eq 0) "$label (exit $code)"
}

Push-Location $root
try {
    Invoke-Native 'cargo test' { cargo test -q --workspace --exclude skyrimxer-er }
    Invoke-Native 'cargo build fake-peer' { cargo build -q -p fake-peer }
    if (-not (Test-Path -LiteralPath $cppExe)) { throw "missing $cppExe; run tools/build.ps1 -Target skse first" }
    Invoke-Native 'C++ selftest' { & $cppExe selftest }

    $region = "Local\SkyrimXER_test_interop_$PID"

    # Starts two peers at once, waits for both, returns their stdout as strings.
    function Invoke-Pair([string]$name, [string]$exeA, [string]$argsA, [string]$exeB, [string]$argsB) {
        $files = @("$outDir\$name-a.log", "$outDir\$name-b.log")
        $a = Start-Process -FilePath $exeA -ArgumentList $argsA -NoNewWindow -PassThru -RedirectStandardOutput $files[0] -RedirectStandardError "$($files[0]).err"
        Start-Sleep -Milliseconds 300
        $b = Start-Process -FilePath $exeB -ArgumentList $argsB -NoNewWindow -PassThru -RedirectStandardOutput $files[1] -RedirectStandardError "$($files[1]).err"
        $a.WaitForExit(); $b.WaitForExit()
        return @((Get-Content -Raw -LiteralPath $files[0]), (Get-Content -Raw -LiteralPath $files[1]))
    }

    Write-Host '== interop A: Rust fake ER crashes -> C++ Skyrim times out (~10 s) ==' -ForegroundColor Cyan
    $er, $sky = Invoke-Pair 'interop-a' $fakeExe "er --seconds 6 --no-bye --region $region" $cppExe "peer --side skyrim --seconds 10 --dodge-every 2 --region $region"
    Check ($sky -match 'CONNECTED: handshake ok with ER') 'C++ Skyrim: handshake with Rust ER'
    Check ($er -match 'CONNECTED: handshake ok with Skyrim') 'Rust ER: handshake with C++ Skyrim'
    Check (($sky -match 'ER heartbeat seq=') -and ($er -match 'Skyrim heartbeat seq=')) 'heartbeat events both ways'
    Check ($sky -match 'LOST: ER .*heartbeat timeout') 'C++ Skyrim: heartbeat timeout after the crash'
    Check ($er -match 'Dodge down \(InputState .*stamina 100.{1,3}80') 'slot sky->er: Rust ER saw the C++ Dodge press and dodged'
    Check (($sky -match 'stamina 100.{1,3}80') -and ($sky -match 'IFrame on') -and ($sky -match 'IFrame off')) 'slot er->sky: C++ Skyrim saw stamina drop + IFrame on/off'
    Check (([regex]::Matches($sky, 'PlayerState stale')).Count -eq 1) 'C++ Skyrim: PlayerState stale exactly once after the crash'
    Check (($sky -match '\[pose] Active on bones=24') -and ($sky -match '\[pose\] Active off .*pelvis swing max=([1-3]\d) deg, bad quats=0\)')) 'pose slot er->sky: C++ Skyrim saw a swinging Active pose with unit quaternions'
    Check (-not ($sky -match 'seq gap|corrupt|MISMATCH') -and -not ($er -match 'seq gap|corrupt|MISMATCH')) 'no seq gaps, corruption or mismatches'

    Write-Host '== interop B: C++ ER exits with Bye -> Rust fake Skyrim goes idle (~6 s) ==' -ForegroundColor Cyan
    $er, $sky = Invoke-Pair 'interop-b' $cppExe "peer --side er --seconds 4 --region $region" $fakeExe "skyrim --seconds 6 --region $region"
    Check ($sky -match 'CONNECTED: handshake ok with ER') 'Rust Skyrim: handshake with C++ ER'
    # The header state (ShuttingDown) is usually seen a tick before the Bye message is read; either way it must end idle.
    Check (($sky -match 'LOST: ER .*shutting down|LOST: ER said Bye') -and ($sky -match 'said Bye \(reason=Quit\)')) 'Rust Skyrim: clean exit + Bye seen, going idle'
    Check ($er -match 'shutting down \(Bye sent: yes\)') 'C++ ER: Bye sent'
    Write-Host "  (peer logs: $outDir)"
} finally {
    Pop-Location
}

if ($failures) { Write-Host "`n$failures check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "`nall tests passed" -ForegroundColor Green
exit 0
