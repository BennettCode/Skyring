//! Minimal file logger using the shared line format (docs/DESIGN.md, "Logging format"):
//! `2026-10-03T14:22:05.123Z [ER] [info] [subsystem] message`

use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock, TryLockError};

static LOG_FILE: OnceLock<Mutex<File>> = OnceLock::new();
/// Set during process exit: other threads may have been killed while holding the lock, so never block on it then.
pub static EXITING: AtomicBool = AtomicBool::new(false);

/// Opens (truncating) `<dir>/skyrimxer_er.log`. Later calls are ignored.
pub fn init(dir: &Path) -> std::io::Result<()> {
    fs::create_dir_all(dir)?;
    let file = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(dir.join("skyrimxer_er.log"))?;
    let _ = LOG_FILE.set(Mutex::new(file));
    Ok(())
}

pub fn write(level: &str, subsystem: &str, message: &str) {
    let Some(file) = LOG_FILE.get() else {
        return;
    };
    let timestamp = chrono::Utc::now().format("%Y-%m-%dT%H:%M:%S%.3fZ");
    // A poisoned lock only means another thread panicked mid-write; the file is still usable.
    let mut file = if EXITING.load(Ordering::Relaxed) {
        match file.try_lock() {
            Ok(f) => f,
            Err(TryLockError::Poisoned(p)) => p.into_inner(),
            Err(TryLockError::WouldBlock) => return,
        }
    } else {
        file.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    };
    let _ = writeln!(file, "{timestamp} [ER] [{level}] [{subsystem}] {message}");
    let _ = file.flush();
}

#[macro_export]
macro_rules! info {
    ($subsystem:literal, $($arg:tt)*) => { $crate::log::write("info", $subsystem, &format!($($arg)*)) };
}

#[macro_export]
macro_rules! error {
    ($subsystem:literal, $($arg:tt)*) => { $crate::log::write("error", $subsystem, &format!($($arg)*)) };
}

#[macro_export]
macro_rules! warn {
    ($subsystem:literal, $($arg:tt)*) => { $crate::log::write("warning", $subsystem, &format!($($arg)*)) };
}
