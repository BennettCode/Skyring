//! skyrimxer-protocol: the Rust half of the shared-memory contract.
//!
//! - [`proto`]: generated from `protocol/schema/messages.toml` (never edit by hand).
//! - [`region`]: open-or-create the named mapping.
//! - [`ring`]: the one-way event rings.
//! - [`link`]: the per-side state machine (heartbeat, handshake, timeout). `skse/src/bridge/Link.cpp` mirrors it in C++.

#[allow(dead_code)]
pub mod proto {
    include!("../generated/skyrimxer_protocol.rs");
}

pub mod link;
pub mod region;
pub mod ring;

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
