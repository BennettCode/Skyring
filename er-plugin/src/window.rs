//! Hidden-but-focused: ER must keep simulating while the player looks at Skyrim (SkyCraft's "hidden, told it's focused").
//! Every frame: tell ER's input layer its window is focused and in the foreground. Once the player is in the world, hide the
//! window; show it again when the player leaves the world, so the title screen stays usable.
//! `visible=1` in skyrimxer_er.cfg (`dev.ps1 -ErVisible`) keeps the window visible (debugging). Notes: docs/research/elden-ring-input.md.

use std::ffi::c_void;

use eldenring::cs::CSWindowImp;
use eldenring::dluid::DLUserInputManagerImpl;
use eldenring::fd4::FD4PadManager;
use fromsoftware_shared::FromStatic;
use windows::Win32::Foundation::HWND;
use windows::Win32::UI::WindowsAndMessaging::{IsWindowVisible, SW_HIDE, SW_SHOW, ShowWindow};

pub struct WindowControl {
    keep_visible: bool,
    hidden: bool,
    warned: bool,
}

impl WindowControl {
    pub fn new() -> Self {
        let keep_visible = crate::config::get().visible;
        crate::info!("window", "hide window when in world: {}", if keep_visible { "no (visible=1)" } else { "yes" });
        Self { keep_visible, hidden: false, warned: false }
    }

    /// FrameBegin. `in_world` = the main player exists.
    pub fn frame(&mut self, in_world: bool) {
        // SAFETY: main thread (task callback).
        unsafe {
            if let Ok(input) = DLUserInputManagerImpl::instance_mut() {
                input.is_game_window_focused = true;
            }
            if let Ok(pads) = FD4PadManager::instance_mut() {
                pads.is_back_ground_window = false;
                pads.exit_foreground_signaled = false;
            }
        }
        let want_hidden = in_world && !self.keep_visible;
        if want_hidden != self.hidden {
            self.set_hidden(want_hidden);
        }
    }

    fn set_hidden(&mut self, hide: bool) {
        // SAFETY: main thread.
        let Some(hwnd) = (unsafe { CSWindowImp::instance() }).ok().map(|w| HWND(w.window_handle as *mut c_void)).filter(|h| !h.is_invalid())
        else {
            if !self.warned {
                self.warned = true;
                crate::error!("window", "CSWindow has no window handle yet; cannot {}", if hide { "hide" } else { "show" });
            }
            return;
        };
        unsafe {
            let _ = ShowWindow(hwnd, if hide { SW_HIDE } else { SW_SHOW });
        }
        self.hidden = hide;
        let visible = unsafe { IsWindowVisible(hwnd) }.as_bool();
        crate::info!("window", "{} window {:#x} (visible now: {visible})", if hide { "hid" } else { "showed" }, hwnd.0 as usize);
    }
}
