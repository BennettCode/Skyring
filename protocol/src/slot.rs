//! Seqlock slots: latest-value state lanes between the game threads (`SLOT_*` blocks in the schema).
//! `skse/src/bridge/Slot.h` is the C++ mirror: keep the two in step.
//!
//! One writer, any number of readers. The struct starts with `seq: u32` (odd = being written; protogen checks this).
//! - Writer: seq+1 (relaxed), release fence, copy the body, seq+2 (release).
//! - Reader: seq (acquire), retry while odd, copy the body, acquire fence, accept if seq is unchanged (at most `SLOT_READ_TRIES` tries).
//!
//! The body is copied as u32 words through `AtomicU32`, so a read racing a write is never undefined behaviour; the seq check
//! throws away any torn copy. Data is **stale** when its `time_ms` is older than `SLOT_STALE_MS` (see [`fresh`]) or the link
//! isn't connected: callers check both.

use std::marker::PhantomData;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering, fence};

use crate::Plain;
use crate::proto::{REGION_SIZE, SLOT_READ_TRIES, SLOT_STALE_MS};
use crate::region::Region;

/// True if slot data written at `time_ms` is recent enough to act on at `now_ms`.
pub fn fresh(time_ms: u64, now_ms: u64) -> bool {
    now_ms.saturating_sub(time_ms) <= SLOT_STALE_MS
}

/// Shared part of writer and reader: the region (kept mapped by the `Arc`) and the slot's offset.
struct Slot<T> {
    region: Arc<Region>,
    offset: usize,
    _t: PhantomData<T>,
}

impl<T: Plain> Slot<T> {
    fn new(region: Arc<Region>, offset: usize) -> Self {
        assert!(offset % 8 == 0 && offset + size_of::<T>() <= REGION_SIZE, "slot at {offset:#x} doesn't fit the region");
        assert!(size_of::<T>() % 4 == 0 && size_of::<T>() >= 4, "slot struct must be whole u32 words");
        Self { region, offset, _t: PhantomData }
    }

    fn words() -> usize {
        size_of::<T>() / 4
    }

    /// Word `i` of the slot (word 0 = `seq`).
    fn word(&self, i: usize) -> &AtomicU32 {
        self.region.u32_at(self.offset + i * 4)
    }
}

pub struct SlotWriter<T> {
    slot: Slot<T>,
    seq: u32,
}

impl<T: Plain> SlotWriter<T> {
    /// There must be only one writer per slot (Skyrim's game thread for InputState, ER's for PlayerState).
    pub fn new(region: Arc<Region>, offset: usize) -> Self {
        let slot = Slot::new(region, offset);
        let mut seq = slot.word(0).load(Ordering::Relaxed);
        if seq % 2 == 1 {
            // A previous writer died mid-write: make it even again so readers stop waiting.
            seq = seq.wrapping_add(1);
            slot.word(0).store(seq, Ordering::Release);
        }
        Self { slot, seq }
    }

    /// Publishes `value` (its own `seq` field is ignored).
    pub fn write(&mut self, value: &T) {
        let src = crate::bytes_of(value);
        let seq = self.slot.word(0);
        seq.store(self.seq.wrapping_add(1), Ordering::Relaxed);
        fence(Ordering::Release);
        for i in 1..Slot::<T>::words() {
            let w = u32::from_le_bytes(src[i * 4..i * 4 + 4].try_into().unwrap());
            self.slot.word(i).store(w, Ordering::Relaxed);
        }
        self.seq = self.seq.wrapping_add(2);
        seq.store(self.seq, Ordering::Release);
    }
}

pub struct SlotReader<T> {
    slot: Slot<T>,
}

impl<T: Plain> SlotReader<T> {
    pub fn new(region: Arc<Region>, offset: usize) -> Self {
        Self { slot: Slot::new(region, offset) }
    }

    /// The latest complete value, or `None` if nothing was ever written or every try was torn.
    pub fn read(&self) -> Option<T> {
        let mut value = T::default();
        let words = Slot::<T>::words();
        for _ in 0..SLOT_READ_TRIES {
            let s1 = self.slot.word(0).load(Ordering::Acquire);
            if s1 == 0 {
                return None;
            }
            if s1 % 2 == 1 {
                std::hint::spin_loop();
                continue;
            }
            // SAFETY: Plain = repr(C) number fields, size is whole words; any bit pattern is valid.
            let dst = unsafe { std::slice::from_raw_parts_mut((&raw mut value).cast::<u32>(), words) };
            for (i, w) in dst.iter_mut().enumerate().skip(1) {
                *w = self.slot.word(i).load(Ordering::Relaxed);
            }
            fence(Ordering::Acquire);
            if self.slot.word(0).load(Ordering::Relaxed) == s1 {
                dst[0] = s1;
                return Some(value);
            }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::{InputState, OFF_SLOT_INPUT, OFF_SLOT_PLAYER, PlayerState};
    use std::sync::atomic::AtomicBool;
    use std::time::{Duration, Instant};

    fn region(tag: &str) -> Arc<Region> {
        Arc::new(Region::open(&format!("Local\\SkyrimXER_test_slot_{tag}_{}", std::process::id())).unwrap())
    }

    #[test]
    fn round_trip_and_never_written() {
        let r = region("rt");
        let reader = SlotReader::<InputState>::new(r.clone(), OFF_SLOT_INPUT);
        assert_eq!(reader.read(), None, "never written");
        let mut writer = SlotWriter::<InputState>::new(r.clone(), OFF_SLOT_INPUT);
        let v = InputState { seq: 999, frame: 7, time_ms: 1234, buttons: 1, move_x: -0.5, cam_yaw: 3.0, ..Default::default() };
        writer.write(&v);
        let got = reader.read().unwrap();
        assert_eq!(got.seq, 2, "seq comes from the slot, not the value");
        assert_eq!(InputState { seq: 999, ..got }, v);
        writer.write(&InputState { frame: 8, ..v });
        assert_eq!(reader.read().unwrap().frame, 8);
        assert_eq!(SlotReader::<PlayerState>::new(r, OFF_SLOT_PLAYER).read(), None, "other slot untouched");
    }

    #[test]
    fn pose_round_trip() {
        use crate::proto::{OFF_SLOT_POSE, POSE_BONE_COUNT, PoseBone, PoseState};
        let r = region("pose");
        let reader = SlotReader::<PoseState>::new(r.clone(), OFF_SLOT_POSE);
        assert_eq!(reader.read(), None, "never written");
        let mut v = PoseState { frame: 3, time_ms: 77, bone_count: POSE_BONE_COUNT, yaw: 1.5, pelvis_offset: [0.1, 0.2, 0.3], ..Default::default() };
        assert_eq!(v.rot.len(), 4 * POSE_BONE_COUNT as usize, "rot = 4 floats per PoseBone");
        for (i, f) in v.rot.iter_mut().enumerate() {
            *f = i as f32 * 0.25;
        }
        SlotWriter::<PoseState>::new(r.clone(), OFF_SLOT_POSE).write(&v);
        let got = reader.read().unwrap();
        assert_eq!(PoseState { seq: 0, ..got }, v);
        let last = PoseBone::RFoot as usize * 4;
        assert_eq!(got.rot[last + 3], (last + 3) as f32 * 0.25, "last bone's w is the last word");
        assert_eq!(SlotReader::<PlayerState>::new(r, OFF_SLOT_PLAYER).read(), None, "player slot untouched");
    }

    #[test]
    fn odd_seq_from_a_dead_writer_is_repaired() {
        let r = region("odd");
        r.u32_at(OFF_SLOT_INPUT).store(5, Ordering::Relaxed);
        let reader = SlotReader::<InputState>::new(r.clone(), OFF_SLOT_INPUT);
        assert_eq!(reader.read(), None, "stuck mid-write");
        let mut writer = SlotWriter::<InputState>::new(r, OFF_SLOT_INPUT);
        assert!(reader.read().is_some(), "even again after the new writer attached");
        writer.write(&InputState { frame: 1, ..Default::default() });
        assert_eq!(reader.read().unwrap().seq, 8);
    }

    #[test]
    fn fresh_limits() {
        assert!(fresh(1000, 1000 + SLOT_STALE_MS));
        assert!(!fresh(1000, 1001 + SLOT_STALE_MS));
        assert!(fresh(1000, 900), "writer clock slightly ahead");
    }

    /// Every field derives from one counter, so a torn copy shows up as fields that disagree.
    fn snapshot(k: u64) -> PlayerState {
        let i = k as i32;
        let f = (k % 100_000) as f32;
        PlayerState {
            flags: k as u32,
            frame: k,
            time_ms: k.wrapping_mul(3),
            hp: i,
            max_hp: i.wrapping_add(1),
            fp: i.wrapping_add(2),
            max_fp: i.wrapping_add(3),
            stamina: i.wrapping_add(4),
            max_stamina: i.wrapping_add(5),
            anim_id: i.wrapping_add(6),
            block_id: i.wrapping_add(7),
            poise: f,
            poise_max: f + 1.0,
            pos: [f + 2.0, f + 3.0, f + 4.0],
            yaw: f + 5.0,
            ..Default::default()
        }
    }

    #[test]
    fn concurrent_reads_are_never_torn() {
        let r = region("torn");
        let stop = Arc::new(AtomicBool::new(false));
        let writer = {
            let (r, stop) = (r.clone(), stop.clone());
            std::thread::spawn(move || {
                let mut w = SlotWriter::<PlayerState>::new(r, OFF_SLOT_PLAYER);
                let mut k = 1u64;
                while !stop.load(Ordering::Relaxed) {
                    w.write(&snapshot(k));
                    k += 1;
                    // Real writers publish once per frame; a tight loop would starve the reader of quiet windows.
                    for _ in 0..200 {
                        std::hint::spin_loop();
                    }
                }
                k
            })
        };
        let reader = SlotReader::<PlayerState>::new(r, OFF_SLOT_PLAYER);
        let (mut reads, mut last) = (0u64, 0u64);
        let end = Instant::now() + Duration::from_millis(500);
        while Instant::now() < end {
            if let Some(s) = reader.read() {
                assert_eq!(PlayerState { seq: 0, ..s }, snapshot(s.frame), "torn read at k={}", s.frame);
                assert!(s.frame >= last, "went backwards");
                last = s.frame;
                reads += 1;
            }
        }
        stop.store(true, Ordering::Relaxed);
        let written = writer.join().unwrap();
        assert!(reads > 1000 && written > 1000, "reads={reads} written={written}");
    }
}
