# Keyboard input and window focus for the dev scripts: lets the agent load into the games and run key tests without the user.
# Keys are sent with SendInput as scan codes (both games read DirectInput/raw scan codes). Safety: before every key the target game
# must be the foreground window; if it isn't (the user clicked elsewhere), every held key is released and the script stops, so keys
# never land in another app. Offline dev use only (CLAUDE.md rule 10); nothing here touches the games' memory.

Set-StrictMode -Version Latest

if (-not ('SxerGameInput' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SxerGameInput {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public InputUnion u; }

    const uint INPUT_KEYBOARD = 1, KEYEVENTF_EXTENDEDKEY = 0x1, KEYEVENTF_KEYUP = 0x2, KEYEVENTF_SCANCODE = 0x8;

    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] static extern void SwitchToThisWindow(IntPtr hwnd, bool altTab);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

    public static uint ForegroundPid() {
        uint pid;
        GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        return pid;
    }

    // Plain SetForegroundWindow, then SwitchToThisWindow (the only one that worked while Chrome Remote Desktop held the foreground,
    // 2026-10-04), then attached to the foreground thread's input, then the Alt-tap trick (Windows' foreground lock).
    public static bool Focus(IntPtr hwnd) {
        if (IsIconic(hwnd)) ShowWindow(hwnd, 9);  // SW_RESTORE
        if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return true;
        SwitchToThisWindow(hwnd, true);
        System.Threading.Thread.Sleep(200);
        if (GetForegroundWindow() == hwnd) return true;
        uint ignored;
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out ignored), me = GetCurrentThreadId();
        if (fg != me) AttachThreadInput(me, fg, true);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        if (fg != me) AttachThreadInput(me, fg, false);
        if (GetForegroundWindow() == hwnd) return true;
        keybd_event(0x12, 0, 0, UIntPtr.Zero);  // Alt down/up unlocks SetForegroundWindow
        keybd_event(0x12, 0, 2, UIntPtr.Zero);
        SetForegroundWindow(hwnd);
        return GetForegroundWindow() == hwnd;
    }

    // Relative mouse motion (games read raw/DirectInput deltas): turns the camera.
    public static bool Mouse(int dx, int dy) {
        var input = new INPUT { type = 0 };
        input.u.mi.dx = dx;
        input.u.mi.dy = dy;
        input.u.mi.dwFlags = 0x0001;  // MOUSEEVENTF_MOVE
        return SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT))) == 1;
    }

    public static bool Key(ushort scan, bool extended, bool up) {
        var input = new INPUT { type = INPUT_KEYBOARD };
        input.u.ki.wScan = scan;
        input.u.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        return SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT))) == 1;
    }
}
'@
}

# Set-1 scan codes. 0x100 = extended key (arrows).
$script:Keys = @{
    Esc = 0x01; '1' = 0x02; '2' = 0x03; '3' = 0x04; '4' = 0x05; Tab = 0x0F; Q = 0x10; W = 0x11; E = 0x12; R = 0x13; Enter = 0x1C
    LCtrl = 0x1D; A = 0x1E; S = 0x1F; D = 0x20; F = 0x21; G = 0x22; LShift = 0x2A; Z = 0x2C; X = 0x2D; C = 0x2E; V = 0x2F
    LAlt = 0x38; Space = 0x39; F1 = 0x3B; F5 = 0x3F; F7 = 0x41; F8 = 0x42; F9 = 0x43; F10 = 0x44; Grave = 0x29
    Up = 0x148; Left = 0x14B; Right = 0x14D; Down = 0x150
}

$script:ProcessNames = @{ skyrim = 'SkyrimSE'; eldenring = 'eldenring' }

function Get-GameProcess([ValidateSet('skyrim', 'eldenring')][string]$Game) {
    Get-Process -Name $script:ProcessNames[$Game] -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } |
        Select-Object -First 1
}

function Test-GameFocused([ValidateSet('skyrim', 'eldenring')][string]$Game) {
    $p = Get-GameProcess $Game
    return [bool]($p -and [SxerGameInput]::ForegroundPid() -eq [uint32]$p.Id)
}

<#
.SYNOPSIS
  Brings the game's window to the front. Returns $true once it is the foreground window (tries for -TimeoutSeconds).
#>
function Set-GameFocus {
    param([ValidateSet('skyrim', 'eldenring')][string]$Game, [int]$TimeoutSeconds = 10)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $p = Get-GameProcess $Game
        if ($p) {
            if (Test-GameFocused $Game) { return $true }
            [void][SxerGameInput]::Focus($p.MainWindowHandle)
            Start-Sleep -Milliseconds 300
            if (Test-GameFocused $Game) { return $true }
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    return $false
}

function Resolve-Key([string]$Name) {
    $match = $script:Keys.Keys | Where-Object { $_ -ieq $Name } | Select-Object -First 1
    if (-not $match) { throw "unknown key '$Name' (known: $(($script:Keys.Keys | Sort-Object) -join ' '))" }
    return [int]$script:Keys[$match]
}

<#
.SYNOPSIS
  Runs a key script on the focused game: "down W; wait 500; tap LShift 60; wait 1500; up W".
  Steps: down K | up K | tap K [ms, default 50] | wait ms | mouse dx dy (relative, turns the camera; split into 20 px moves).
  Stops (keys released) if the game loses focus.
#>
function Send-GameKeys {
    param([ValidateSet('skyrim', 'eldenring')][string]$Game, [string]$Script, [switch]$NoFocus)
    if (-not $NoFocus -and -not (Set-GameFocus $Game)) { throw "could not bring $Game to the front; click its window once and rerun" }
    $held = [System.Collections.Generic.List[int]]::new()
    $send = {
        param([int]$code, [bool]$up)
        if (-not (Test-GameFocused $Game)) { throw "$Game lost focus; stopped the key script (keys released)" }
        if (-not [SxerGameInput]::Key([uint16]($code -band 0xFF), [bool]($code -band 0x100), $up)) { throw 'SendInput failed' }
        if ($up) { [void]$held.Remove($code) } elseif (-not $held.Contains($code)) { $held.Add($code) }
    }
    try {
        foreach ($step in ($Script -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
            $parts = $step -split '\s+'
            switch ($parts[0].ToLowerInvariant()) {
                'down' { & $send (Resolve-Key $parts[1]) $false }
                'up' { & $send (Resolve-Key $parts[1]) $true }
                'tap' {
                    $code = Resolve-Key $parts[1]
                    $ms = if ($parts.Count -gt 2) { [int]$parts[2] } else { 50 }
                    & $send $code $false
                    Start-Sleep -Milliseconds $ms
                    & $send $code $true
                }
                'wait' { Start-Sleep -Milliseconds ([int]$parts[1]) }
                'mouse' {
                    $dx = [int]$parts[1]; $dy = if ($parts.Count -gt 2) { [int]$parts[2] } else { 0 }
                    $n = [math]::Max(1, [math]::Ceiling([math]::Max([math]::Abs($dx), [math]::Abs($dy)) / 20))
                    for ($i = 0; $i -lt $n; $i++) {
                        if (-not (Test-GameFocused $Game)) { throw "$Game lost focus; stopped the key script (keys released)" }
                        [void][SxerGameInput]::Mouse([int]($dx / $n), [int]($dy / $n))
                        Start-Sleep -Milliseconds 10
                    }
                }
                default { throw "unknown step '$step' (use: down K; up K; tap K [ms]; wait ms; mouse dx dy)" }
            }
            Write-Host ("  [keys] {0:HH:mm:ss.fff} {1}" -f (Get-Date), $step)
        }
    } finally {
        foreach ($code in @($held)) { [void][SxerGameInput]::Key([uint16]($code -band 0xFF), [bool]($code -band 0x100), $true) }
    }
}

Export-ModuleMember -Function Get-GameProcess, Test-GameFocused, Set-GameFocus, Send-GameKeys
