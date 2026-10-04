//! skyrimxer-protocol: the Rust half of the shared-memory contract.
//!
//! - [`proto`]: generated from `protocol/schema/messages.toml` (never edit by hand).
//! - [`region`]: open-or-create the named mapping.
//! - [`ring`]: the one-way event rings.
//! - [`link`]: the per-side state machine (heartbeat, handshake, timeout). `skse/src/bridge/Link.cpp` mirrors it in C++.
//! - [`coords`]: Skyrim ⇄ ER deltas in the character frame (measured conventions). `skse/src/bridge/Coords.h` mirrors it.
//! - [`slot`]: seqlock slots (latest-value state lanes for the game threads). `skse/src/bridge/Slot.h` mirrors it.
//! - [`rig`]: pose retarget math (quaternions, ER → Skyrim model basis, bind deltas).

#[allow(dead_code)]
pub mod proto {
    include!("../generated/skyrimxer_protocol.rs");
}

pub mod coords;
pub mod link;
pub mod region;
pub mod rig;
pub mod ring;
pub mod slot;

/// Plain-old-data protocol struct: `repr(C)`, number fields only, no implicit padding, all-zero is valid.
///
/// # Safety
/// Only implemented by generated code, which checks the layout.
pub unsafe trait Plain: Copy + Default + 'static {}

/// A payload that travels in a ring, tagged with its [`proto::MsgType`].
pub trait Message: Plain {
    const TYPE: proto::MsgType;
}

pub fn bytes_of<T: Plain>(value: &T) -> &[u8] {
    // SAFETY: Plain types have no padding, so every byte is initialised.
    unsafe { std::slice::from_raw_parts((value as *const T).cast::<u8>(), size_of::<T>()) }
}

/// Copies a `T` out of `bytes`; `None` if there are too few bytes.
pub fn read_plain<T: Plain>(bytes: &[u8]) -> Option<T> {
    (bytes.len() >= size_of::<T>())
        // SAFETY: length checked; any bit pattern is a valid Plain value.
        .then(|| unsafe { std::ptr::read_unaligned(bytes.as_ptr().cast::<T>()) })
}

/// Milliseconds since boot (`GetTickCount64`): the clock both sides use for heartbeats.
pub fn now_ms() -> u64 {
    unsafe { windows::Win32::System::SystemInformation::GetTickCount64() }
}

/// Microseconds on the QueryPerformanceCounter clock: one clock for every process on the machine, for interpolation stamps
/// (`time_us`). `skse/src/bridge/Link.cpp` NowUs is the C++ twin.
pub fn now_us() -> u64 {
    use std::sync::OnceLock;
    use windows::Win32::System::Performance::{QueryPerformanceCounter, QueryPerformanceFrequency};
    static FREQ: OnceLock<u64> = OnceLock::new();
    let freq = *FREQ.get_or_init(|| {
        let mut f = 0i64;
        // SAFETY: plain Win32 call writing into our local.
        let _ = unsafe { QueryPerformanceFrequency(&mut f) };
        f.max(1) as u64
    });
    let mut c = 0i64;
    // SAFETY: as above.
    let _ = unsafe { QueryPerformanceCounter(&mut c) };
    (c as u128 * 1_000_000 / freq as u128) as u64
}
