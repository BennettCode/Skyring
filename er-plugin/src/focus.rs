//! Focus probe (`probe=1`, P3 step 2 Stage 3): every focus-related input flag ER keeps, logged whenever it changes, at FrameBegin
//! (before our spoof in window.rs) and at WorldChrMan_Prepare (after PadStep). Finds which flag still blocks the player's action
//! requests when ER is not the foreground window. Read-only. Unnamed bools are read next to their named neighbours in the
//! eldenring-rs layout (FD4PadManager unk10 / unk2fa, DLUserInputManagerImpl unk889 / unk88a / unk88c).

use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, Ordering};

use eldenring::cs::CSWindowImp;
use eldenring::dluid::DLUserInputManagerImpl;
use eldenring::fd4::FD4PadManager;
use fromsoftware_shared::FromStatic;
use windows::Win32::UI::WindowsAndMessaging::GetForegroundWindow;

const MAX_LINES: u32 = 300;
static LINES: AtomicU32 = AtomicU32::new(0);
static LAST: Mutex<[String; 2]> = Mutex::new([String::new(), String::new()]);

/// Byte at `base + delta` (an unnamed bool next to a named field).
///
/// # Safety
/// `base` must point into a live object with at least `delta + 1` readable bytes after it.
unsafe fn byte_at(base: *const bool, delta: isize) -> u8 {
    unsafe { *(base as *const u8).offset(delta) }
}

fn state() -> String {
    let b = |v: bool| v as u8;
    // SAFETY: main thread (task callback); the singletons live for the whole game.
    unsafe {
        let hwnd = CSWindowImp::instance().map(|w| w.window_handle).unwrap_or(0);
        let fg = GetForegroundWindow().0 as isize == hwnd && hwnd != 0;
        let mut out = format!("fg={}", b(fg));
        if let Ok(pm) = FD4PadManager::instance() {
            let bg = &pm.is_back_ground_window as *const bool;
            let wh = &pm.window_handle as *const isize as *const bool;
            out.push_str(&format!(
                " pm[u10={} exit_fg={} bg={} u2fa={}]",
                byte_at(wh, 8),
                b(pm.exit_foreground_signaled),
                b(pm.is_back_ground_window),
                byte_at(bg, 1)
            ));
            out.push_str(" pads[");
            for map in pm.pad_entry_map_list.iter() {
                for pair in map.as_ref().iter() {
                    let pad = pair.second.entry.as_ref();
                    out.push_str(&format!("e{}p{} ", b(pair.second.enable_use), b(pad.allow_polling)));
                }
            }
            out.push(']');
        }
        if let Ok(uim) = DLUserInputManagerImpl::instance() {
            let co = &uim.is_co_initialized as *const bool;
            let sce = &uim.use_lib_sce_pad as *const bool;
            out.push_str(&format!(
                " uim[focused={} u889={} u88a={} u88c={} fg_pad={} fg_kb={} fg_mouse={}]",
                b(uim.is_game_window_focused),
                byte_at(co, 1),
                byte_at(co, 2),
                byte_at(sce, 1),
                b(uim.set_foreground_pad),
                b(uim.set_foreground_keyboard),
                b(uim.set_foreground_mouse)
            ));
        }
        out
    }
}

/// Logs the focus state when it differs from the last one seen at this point (`at` 0 = FrameBegin, 1 = WorldChrMan_Prepare).
pub fn sample(at: usize, name: &str) {
    let s = state();
    let mut last = LAST.lock().unwrap_or_else(|e| e.into_inner());
    if last[at] != s {
        if LINES.fetch_add(1, Ordering::Relaxed) < MAX_LINES {
            let frame = crate::bridge::FRAMES.load(Ordering::Relaxed);
            crate::info!("focus", "frame={frame} {name}: {s}");
        }
        last[at] = s;
    }
}
