//! skyrimxer_er: the Elden Ring half of Skyrim X Elden Ring. Loaded by me3 (see me3/skyrim-x-er.me3).
//!
//! Phase 1: prove the DLL loads, eldenring-rs accepts this game version, and a per-frame task runs.

mod log;

use std::ffi::c_void;
use std::panic;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use eldenring::cs::{CSTaskGroupIndex, CSTaskImp, WorldChrMan};
use eldenring::fd4::FD4TaskData;
use fromsoftware_shared::{FromStatic, SharedTaskImpExt};
use windows::Win32::Foundation::HMODULE;
use windows::Win32::System::LibraryLoader::GetModuleFileNameW;

const DLL_PROCESS_ATTACH: u32 = 1;

/// # Safety
/// Called by the Windows loader only.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn DllMain(hmodule: HMODULE, reason: u32, _reserved: *mut c_void) -> bool {
    if reason == DLL_PROCESS_ATTACH {
        // Never do real work under the loader lock; hand off to a thread.
        let module = hmodule.0 as usize;
        std::thread::spawn(move || init(module));
    }
    true
}

fn init(module: usize) {
    let log_dir = module_dir(module).map(|dir| dir.join("logs"));
    if let Some(dir) = &log_dir {
        let _ = log::init(dir);
    }
    panic::set_hook(Box::new(|info| error!("panic", "{info}")));

    info!(
        "core",
        "skyrimxer_er v{} loaded into {}",
        env!("CARGO_PKG_VERSION"),
        std::env::current_exe().map(|p| p.display().to_string()).unwrap_or_default()
    );

    // eldenring-rs resolves its RVA table here and panics if eldenring.exe isn't a supported version.
    let result = panic::catch_unwind(|| CSTaskImp::wait_for_instance(Duration::MAX));
    let task = match result {
        Ok(Ok(task)) => task,
        Ok(Err(e)) => return error!("core", "CSTaskImp unavailable: {e:?}"),
        Err(_) => return error!("core", "eldenring-rs rejected this game version; mod disabled"),
    };
    info!("core", "game version supported, task system ready");

    let player_present = AtomicBool::new(false);
    task.run_recurring(
        move |_: &FD4TaskData| {
            let present = unsafe { WorldChrMan::instance() }
                .map(|world| world.main_player.is_some())
                .unwrap_or(false);
            if player_present.swap(present, Ordering::Relaxed) != present {
                info!("core", "main player {}", if present { "spawned (in world)" } else { "gone (menu/loading)" });
            }
        },
        CSTaskGroupIndex::FrameBegin,
    );
    info!("core", "per-frame task registered (FrameBegin)");
}

/// Directory containing this DLL, so logs land next to the build output regardless of the game's working directory.
fn module_dir(module: usize) -> Option<PathBuf> {
    let mut buf = [0u16; 1024];
    let len = unsafe { GetModuleFileNameW(Some(HMODULE(module as *mut c_void)), &mut buf) } as usize;
    if len == 0 || len >= buf.len() {
        return None;
    }
    PathBuf::from(String::from_utf16_lossy(&buf[..len])).parent().map(PathBuf::from)
}
