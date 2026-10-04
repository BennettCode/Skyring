//! The named shared-memory region (`proto::REGION_NAME`). Either side may create it; the other opens it.

use std::sync::atomic::{AtomicU32, AtomicU64};

use windows::Win32::Foundation::{CloseHandle, ERROR_ALREADY_EXISTS, GetLastError, HANDLE, INVALID_HANDLE_VALUE};
use windows::Win32::System::Memory::{
    CreateFileMappingW, FILE_MAP_ALL_ACCESS, MEMORY_MAPPED_VIEW_ADDRESS, MapViewOfFile, PAGE_READWRITE, UnmapViewOfFile,
};
use windows::core::HSTRING;

use crate::proto::REGION_SIZE;

pub struct Region {
    mapping: HANDLE,
    base: *mut u8,
    /// True if this process created the mapping (and must initialise the shared header fields).
    pub created: bool,
}

// SAFETY: the mapping is process-wide memory; all shared accesses go through atomics.
unsafe impl Send for Region {}
// SAFETY: as above; game threads share it through `Arc<Region>` (slots) while the link thread uses the rings.
unsafe impl Sync for Region {}

impl Region {
    /// Opens the mapping `name`, creating it (zero-filled) if it doesn't exist yet. Maps exactly `REGION_SIZE` bytes,
    /// so an existing mapping that is too small fails here instead of faulting later.
    pub fn open(name: &str) -> Result<Self, String> {
        let size = REGION_SIZE as u64;
        let mapping = unsafe {
            CreateFileMappingW(INVALID_HANDLE_VALUE, None, PAGE_READWRITE, (size >> 32) as u32, size as u32, &HSTRING::from(name))
        }
        .map_err(|e| format!("CreateFileMappingW({name}) failed: {e}"))?;
        let created = unsafe { GetLastError() } != ERROR_ALREADY_EXISTS;
        let view = unsafe { MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, REGION_SIZE) };
        if view.Value.is_null() {
            let err = windows::core::Error::from_win32();
            unsafe {
                let _ = CloseHandle(mapping);
            }
            return Err(format!("MapViewOfFile({name}, {REGION_SIZE:#x} bytes) failed: {err}"));
        }
        Ok(Self { mapping, base: view.Value.cast(), created })
    }

    pub fn base(&self) -> *mut u8 {
        self.base
    }

    /// The u32 at byte `offset`. Panics if out of range or misaligned (a programming error, not a runtime condition).
    pub fn u32_at(&self, offset: usize) -> &AtomicU32 {
        assert!(offset % 4 == 0 && offset + 4 <= REGION_SIZE);
        // SAFETY: in bounds, aligned, and the mapping outlives `self`.
        unsafe { AtomicU32::from_ptr(self.base.add(offset).cast()) }
    }

    pub fn u64_at(&self, offset: usize) -> &AtomicU64 {
        assert!(offset % 8 == 0 && offset + 8 <= REGION_SIZE);
        // SAFETY: as above.
        unsafe { AtomicU64::from_ptr(self.base.add(offset).cast()) }
    }
}

impl Drop for Region {
    fn drop(&mut self) {
        unsafe {
            let _ = UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS { Value: self.base.cast() });
            let _ = CloseHandle(self.mapping);
        }
    }
}
