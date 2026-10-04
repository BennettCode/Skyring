//! Keeps the hidden ER character parked (P4 step 4). Every dodge moves it in ER's world, so over many rolls it wandered into walls and
//! enemies. The parking spot (anchor) is the character's position whenever neither a dodge nor Skyrim's input moves it (walking it
//! yourself with `-ErVisible` moves the anchor along); once that has been true for RETURN_AFTER frames (so chained rolls aren't cut),
//! it is put back on the anchor: physics position + `chr_proxy_pos_update_requested`. It then only needs a few metres of free ground.
//! ChrIns_PostPhysics, main thread only.

use std::sync::atomic::Ordering;

use eldenring::position::HavokPosition;

use crate::{bridge, game, remote};

/// Frames without a dodge animation before the character goes back to the anchor.
const RETURN_AFTER: u32 = 10;
/// Farther than this from the anchor = a real move (loading, a grace warp, ...): take it as the new anchor instead.
const MAX_RETURN_M: f32 = 40.0;

pub struct Park {
    anchor: Option<HavokPosition>,
    /// A dodge moved the character away from the anchor and it hasn't been returned yet.
    away: bool,
    calm_frames: u32,
}

fn is_dodge_anim(anim: i32) -> bool {
    anim >= 0 && (anim % 1_000_000) / 1000 == 27
}

impl Park {
    pub fn new() -> Self {
        Self { anchor: None, away: false, calm_frames: 0 }
    }

    pub fn run(&mut self) {
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.anchor = None;
            self.away = false;
            return;
        };
        let s = game::snapshot(player);
        let pos = player.chr_ins.modules.physics.position;
        if is_dodge_anim(s.anim_id) || remote::DRIVING.load(Ordering::Relaxed) {
            if !self.away && self.anchor.is_none() {
                self.anchor = Some(pos);
            }
            self.away = true;
            self.calm_frames = 0;
            return;
        }
        if !self.away {
            self.anchor = Some(pos);
            return;
        }
        self.calm_frames += 1;
        if self.calm_frames < RETURN_AFTER {
            return;
        }
        self.away = false;
        let Some(anchor) = self.anchor else { return };
        let (dx, dy, dz) = (pos.0 - anchor.0, pos.1 - anchor.1, pos.2 - anchor.2);
        let dist = (dx * dx + dy * dy + dz * dz).sqrt();
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        if dist > MAX_RETURN_M {
            crate::info!("park", "frame={frame} {dist:.1} m from the parking spot: taking the new position as the spot");
            self.anchor = Some(pos);
            return;
        }
        let physics = &mut player.chr_ins.modules.physics;
        physics.position = anchor;
        physics.chr_proxy_pos_update_requested = true;
        crate::info!(
            "park",
            "frame={frame} dodge over: back to the parking spot ({dist:.2} m) anim={} at ({:.2},{:.2},{:.2})",
            s.anim_id,
            anchor.0,
            anchor.1,
            anchor.2
        );
    }
}
