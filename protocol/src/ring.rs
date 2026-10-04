//! One-way event rings: exactly one writer process and one reader process each.
//!
//! `write_pos` / `read_pos` count total bytes ever written / consumed (u64, never wrap). The writer copies a message,
//! then publishes `write_pos` with release; the reader loads it with acquire, copies, then publishes `read_pos`.
//! Each message is a `MsgHeader` + payload, padded to `MSG_ALIGN`; a message may wrap around the end of the buffer.

use std::mem::offset_of;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use crate::proto::{MSG_ALIGN, MSG_MAX_PAYLOAD, MsgHeader, RING_CAPACITY, RingHeader};
use crate::{Plain, bytes_of, read_plain};

const MSG_HEADER: usize = size_of::<MsgHeader>();

fn total_len(payload: usize) -> u64 {
    let align = MSG_ALIGN as usize;
    ((MSG_HEADER + payload).div_ceil(align) * align) as u64
}

/// Raw view of one ring inside a mapped region.
struct Ring {
    header: *mut u8,
    data: *mut u8,
}

impl Ring {
    fn write_pos(&self) -> &AtomicU64 {
        unsafe { AtomicU64::from_ptr(self.header.add(offset_of!(RingHeader, write_pos)).cast()) }
    }
    fn read_pos(&self) -> &AtomicU64 {
        unsafe { AtomicU64::from_ptr(self.header.add(offset_of!(RingHeader, read_pos)).cast()) }
    }
    fn dropped(&self) -> &AtomicU32 {
        unsafe { AtomicU32::from_ptr(self.header.add(offset_of!(RingHeader, dropped)).cast()) }
    }

    fn copy_in(&self, pos: u64, src: &[u8]) {
        let cap = RING_CAPACITY as usize;
        let start = (pos % cap as u64) as usize;
        let first = src.len().min(cap - start);
        unsafe {
            std::ptr::copy_nonoverlapping(src.as_ptr(), self.data.add(start), first);
            std::ptr::copy_nonoverlapping(src.as_ptr().add(first), self.data, src.len() - first);
        }
    }

    fn copy_out(&self, pos: u64, dst: &mut [u8]) {
        let cap = RING_CAPACITY as usize;
        let start = (pos % cap as u64) as usize;
        let first = dst.len().min(cap - start);
        unsafe {
            std::ptr::copy_nonoverlapping(self.data.add(start), dst.as_mut_ptr(), first);
            std::ptr::copy_nonoverlapping(self.data, dst.as_mut_ptr().add(first), dst.len() - first);
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum PushError {
    /// Not enough free space (the reader is slow or gone). Counted in `RingHeader::dropped`.
    Full,
    TooBig,
}

pub struct RingWriter {
    ring: Ring,
    seq: u32,
}

// SAFETY: points into a mapped region owned alongside it (see link::Attached).
unsafe impl Send for RingWriter {}

impl RingWriter {
    /// # Safety
    /// `base` must point to a mapped region of `REGION_SIZE` bytes that outlives the writer, and this must be the only
    /// writer of that ring.
    pub unsafe fn new(base: *mut u8, header_off: usize, data_off: usize) -> Self {
        Self { ring: Ring { header: unsafe { base.add(header_off) }, data: unsafe { base.add(data_off) } }, seq: 0 }
    }

    /// Restart sequence numbers at 1 (called on attach).
    pub fn reset_seq(&mut self) {
        self.seq = 0;
    }

    pub fn dropped(&self) -> u32 {
        self.ring.dropped().load(Ordering::Relaxed)
    }

    /// Sends one message; returns its seq.
    pub fn push(&mut self, msg_type: u16, payload: &[u8]) -> Result<u32, PushError> {
        if payload.len() > MSG_MAX_PAYLOAD as usize {
            return Err(PushError::TooBig);
        }
        let total = total_len(payload.len());
        let w = self.ring.write_pos().load(Ordering::Relaxed);
        let r = self.ring.read_pos().load(Ordering::Acquire);
        let used = w.wrapping_sub(r);
        if used > RING_CAPACITY as u64 || RING_CAPACITY as u64 - used < total {
            self.ring.dropped().fetch_add(1, Ordering::Relaxed);
            return Err(PushError::Full);
        }
        self.seq = self.seq.wrapping_add(1).max(1);
        let header = MsgHeader { msg_type, size: payload.len() as u16, seq: self.seq };
        self.ring.copy_in(w, bytes_of(&header));
        self.ring.copy_in(w + MSG_HEADER as u64, payload);
        self.ring.write_pos().store(w + total, Ordering::Release);
        Ok(self.seq)
    }

    pub fn send<T: crate::Message>(&mut self, payload: &T) -> Result<u32, PushError> {
        self.push(T::TYPE as u16, bytes_of(payload))
    }
}

/// A message read from a ring. `payload` borrows the reader's buffer.
pub struct Received<'a> {
    pub msg_type: u16,
    pub seq: u32,
    pub payload: &'a [u8],
}

impl Received<'_> {
    pub fn decode<T: Plain>(&self) -> Option<T> {
        read_plain(self.payload)
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum PopError {
    /// The ring held garbage (bad size or positions). The reader skipped to the writer's position.
    Corrupt { skipped: u64 },
}

pub struct RingReader {
    ring: Ring,
    buf: Box<[u8]>,
}

// SAFETY: as for RingWriter.
unsafe impl Send for RingReader {}

impl RingReader {
    /// # Safety
    /// As for [`RingWriter::new`], and this must be the only reader of that ring.
    pub unsafe fn new(base: *mut u8, header_off: usize, data_off: usize) -> Self {
        Self {
            ring: Ring { header: unsafe { base.add(header_off) }, data: unsafe { base.add(data_off) } },
            buf: vec![0u8; MSG_MAX_PAYLOAD as usize].into_boxed_slice(),
        }
    }

    /// Discards everything unread (stale messages from before this side attached). Returns the bytes skipped.
    pub fn skip_all(&mut self) -> u64 {
        let w = self.ring.write_pos().load(Ordering::Acquire);
        let r = self.ring.read_pos().swap(w, Ordering::AcqRel);
        w.wrapping_sub(r)
    }

    pub fn pop(&mut self) -> Option<Result<Received<'_>, PopError>> {
        let r = self.ring.read_pos().load(Ordering::Relaxed);
        let w = self.ring.write_pos().load(Ordering::Acquire);
        if w == r {
            return None;
        }
        let avail = w.wrapping_sub(r);
        let mut raw = [0u8; MSG_HEADER];
        let header = (avail >= MSG_HEADER as u64 && avail <= RING_CAPACITY as u64)
            .then(|| {
                self.ring.copy_out(r, &mut raw);
                read_plain::<MsgHeader>(&raw).unwrap()
            })
            .filter(|h| h.size as u32 <= MSG_MAX_PAYLOAD && total_len(h.size as usize) <= avail);
        let Some(header) = header else {
            self.ring.read_pos().store(w, Ordering::Release);
            return Some(Err(PopError::Corrupt { skipped: avail }));
        };
        let size = header.size as usize;
        self.ring.copy_out(r + MSG_HEADER as u64, &mut self.buf[..size]);
        self.ring.read_pos().store(r + total_len(size), Ordering::Release);
        Some(Ok(Received { msg_type: header.msg_type, seq: header.seq, payload: &self.buf[..size] }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::{OFF_RING_SKY_TO_ER, OFF_RING_SKY_TO_ER_DATA};
    use crate::region::Region;

    fn region(tag: &str) -> Region {
        Region::open(&format!("Local\\SkyrimXER_test_ring_{tag}_{}", std::process::id())).unwrap()
    }

    fn pair(region: &Region) -> (RingWriter, RingReader) {
        unsafe {
            (
                RingWriter::new(region.base(), OFF_RING_SKY_TO_ER, OFF_RING_SKY_TO_ER_DATA),
                RingReader::new(region.base(), OFF_RING_SKY_TO_ER, OFF_RING_SKY_TO_ER_DATA),
            )
        }
    }

    #[test]
    fn roundtrip_and_wraparound() {
        let region = region("wrap");
        let (mut tx, mut rx) = pair(&region);
        // 3000 messages of 13..=113 bytes go round the 64 KiB buffer several times, wrapping mid-message.
        for i in 0..3000u32 {
            let payload: Vec<u8> = (0..(13 + i % 101)).map(|b| (b as u32 ^ i) as u8).collect();
            assert_eq!(tx.push(7, &payload), Ok(i + 1));
            let got = rx.pop().unwrap().unwrap();
            assert_eq!((got.msg_type, got.seq, got.payload), (7, i + 1, payload.as_slice()));
        }
        assert!(rx.pop().is_none());
    }

    #[test]
    fn full_ring_drops_and_counts() {
        let region = region("full");
        let (mut tx, mut rx) = pair(&region);
        let payload = [0u8; 1016]; // 1024 bytes per message → exactly 64 fit
        for _ in 0..64 {
            tx.push(1, &payload).unwrap();
        }
        assert_eq!(tx.push(1, &payload), Err(PushError::Full));
        assert_eq!(tx.dropped(), 1);
        assert!(rx.pop().unwrap().is_ok());
        assert!(tx.push(1, &payload).is_ok(), "space freed by the reader is reusable");
        assert_eq!(rx.skip_all(), 64 * 1024);
        assert!(rx.pop().is_none());
    }

    #[test]
    fn oversized_payload_is_rejected() {
        let region = region("big");
        let (mut tx, _) = pair(&region);
        assert_eq!(tx.push(1, &vec![0u8; MSG_MAX_PAYLOAD as usize + 1]), Err(PushError::TooBig));
    }

    #[test]
    fn corrupt_header_resyncs() {
        let region = region("corrupt");
        let (mut tx, mut rx) = pair(&region);
        tx.push(1, &[1, 2, 3]).unwrap();
        // Scribble a payload size bigger than the max into the message header.
        unsafe { *region.base().add(OFF_RING_SKY_TO_ER_DATA + 2).cast::<u16>() = 0xFFFF };
        assert_eq!(rx.pop().unwrap().err(), Some(PopError::Corrupt { skipped: 16 }));
        tx.push(2, &[4]).unwrap();
        assert_eq!(rx.pop().unwrap().unwrap().msg_type, 2);
    }
}
