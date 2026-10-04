//! The ER end of the shared-memory link: a thread ticks `skyrimxer_protocol::link::Link` every LINK_TICK_MS.
//! While Skyrim is absent or lost, ER just idles: the game threads see `connected() == false` through [`shared`] and inject nothing.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use skyrimxer_protocol::link::{Identity, Level, Link, LinkShared, Side};
use skyrimxer_protocol::now_ms;
use skyrimxer_protocol::proto::LINK_TICK_MS;

/// FrameBegin tasks run so far; sent in Heartbeat events so Skyrim's log shows whether ER's game loop is advancing.
pub static FRAMES: AtomicU64 = AtomicU64::new(0);

static LINK: Mutex<Option<Link>> = Mutex::new(None);
/// Region + connected flag for the game-thread tasks (remote.rs): never take LINK's lock from a task.
static SHARED: OnceLock<Arc<LinkShared>> = OnceLock::new();

/// `None` until `start()` ran (the version check passed).
pub fn shared() -> Option<&'static Arc<LinkShared>> {
    SHARED.get()
}

/// eldenring-rs only accepts this exe version, and the link starts after that check passed.
const GAME_VERSION: [u16; 4] = [2, 7, 1, 0];

fn plugin_version() -> [u16; 3] {
    let mut parts = env!("CARGO_PKG_VERSION").split('.').map(|p| p.parse().unwrap_or(0));
    [(); 3].map(|_| parts.next().unwrap_or(0))
}

pub fn start() {
    let log = Box::new(|level: Level, msg: &str| {
        let level = match level {
            Level::Info => "info",
            Level::Warn => "warning",
            Level::Error => "error",
        };
        crate::log::write(level, "link", msg);
    });
    let link = Link::new(Side::EldenRing, Identity { plugin_version: plugin_version(), game_version: GAME_VERSION }, log);
    let _ = SHARED.set(link.shared());
    *LINK.lock().unwrap_or_else(|p| p.into_inner()) = Some(link);

    std::thread::spawn(|| loop {
        if let Some(link) = LINK.lock().unwrap_or_else(|p| p.into_inner()).as_mut() {
            link.tick(now_ms(), FRAMES.load(Ordering::Relaxed));
        }
        std::thread::sleep(Duration::from_millis(LINK_TICK_MS));
    });
    crate::info!("core", "link thread started (tick {LINK_TICK_MS} ms); waiting for Skyrim");
}

/// Called from DllMain on process detach. Other threads are already gone, possibly mid-lock, so never block here.
pub fn on_process_exit() {
    crate::log::EXITING.store(true, Ordering::Relaxed);
    if let Ok(mut guard) = LINK.try_lock() {
        if let Some(link) = guard.as_mut() {
            link.shutdown();
        }
    }
}
