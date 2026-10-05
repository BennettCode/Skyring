//! Pins the hidden ER character to its spot (P4 step 4b). ER walls and enemies used to shorten rolls: the character really moved in
//! ER's world, and Skyrim copies its movement. Now, every frame after ER's physics has moved it by this frame's step, the horizontal
//! step is added to a *virtual* position and the character is put back on its spot (physics position + `chr_proxy_pos_update_requested`).
//! It never gets further than one frame's step (~0.1 m) from the spot; Skyrim gets the virtual position in PlayerState.pos
//! (`remote::publish_state`), so its deltas are the full roll. Y stays ER's (slopes, gravity). A jump > MAX_STEP_M in one frame
//! (loading, a grace warp) takes the new position as the spot. `pin=0` in skyrimxer_er.cfg (dev.ps1 `-ErNoPin`): no writes, the
//! character moves freely (virtual = real). ChrIns_PostPhysics, main thread only.

use std::sync::Mutex;
use std::sync::atomic::Ordering;

use eldenring::position::HavokPosition;

use crate::{bridge, config, game};

/// Farther than this in one frame = a real move (loading, a grace warp, ...), never a dodge step: it becomes the new spot.
/// Skyrim ignores jumps of the same size (Movement.cpp kMaxStepM).
const MAX_STEP_M: f32 = 1.5;
/// Smaller steps are left alone (no position write while standing still).
const MIN_STEP_M: f32 = 0.001;

/// Virtual position of this frame (Havok, Y-up, metres); `None` while there is no player.
static VIRTUAL: Mutex<Option<[f32; 3]>> = Mutex::new(None);

/// The position PlayerState publishes: where the character would be if it wasn't pinned.
pub fn virtual_pos() -> Option<[f32; 3]> {
    *VIRTUAL.lock().unwrap_or_else(|p| p.into_inner())
}

pub(crate) fn is_dodge_anim(anim: i32) -> bool {
    anim >= 0 && (anim % 1_000_000) / 1000 == 27
}

/// One dodge animation, for the `[park] dodge` line.
struct Dodge {
    anim: i32,
    frames: u32,
    start: [f32; 3],
    max_drift: f32,
}

pub struct Park {
    /// ER is in a riding anim (game::is_foreign_anim): logged once per ride.
    riding: bool,
    pin: bool,
    /// The spot (pinned) or last frame's position (not pinned).
    anchor: Option<HavokPosition>,
    virt: [f32; 3],
    dodge: Option<Dodge>,
}

impl Park {
    pub fn new() -> Self {
        let pin = config::get().pin;
        crate::info!("park", "{}", if pin { "pin on: the character stays on its spot, Skyrim gets its virtual position" } else { "pin OFF (pin=0): the character moves freely" });
        Self { pin, anchor: None, virt: [0.0; 3], dodge: None, riding: false }
    }

    pub fn run(&mut self) {
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.anchor = None;
            self.dodge = None;
            *VIRTUAL.lock().unwrap_or_else(|p| p.into_inner()) = None;
            return;
        };
        let anim = game::snapshot(player).anim_id;
        let riding = game::is_riding(player);
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        let physics = &mut player.chr_ins.modules.physics;
        let pos = physics.position;
        let before = self.virt;
        let step = match self.anchor {
            None => {
                crate::info!("park", "frame={frame} spot ({:.2},{:.2},{:.2})", pos.0, pos.1, pos.2);
                self.virt = [pos.0, pos.1, pos.2];
                self.anchor = Some(pos);
                0.0
            }
            Some(a) => {
                let (dx, dz) = (pos.0 - a.0, pos.2 - a.2);
                let step = dx.hypot(dz);
                if step > MAX_STEP_M {
                    crate::info!("park", "frame={frame} moved {step:.1} m in one frame: new spot ({:.2},{:.2},{:.2})", pos.0, pos.1, pos.2);
                    self.virt = [pos.0, pos.1, pos.2];
                    self.anchor = Some(pos);
                    self.dodge = None;
                    0.0
                } else if riding {
                    // Riding (never asked for): Skyrim must not follow the horse. Take the new place as the spot, virtual position unchanged.
                    if !self.riding {
                        self.riding = true;
                        crate::warn!("park", "frame={frame} ER is on Torrent (anim {anim}): its movement is not passed to Skyrim");
                    }
                    self.anchor = Some(pos);
                    0.0
                } else {
                    if self.riding {
                        self.riding = false;
                        crate::info!("park", "frame={frame} ER off the horse (anim {anim})");
                    }
                    self.virt = [self.virt[0] + dx, pos.1, self.virt[2] + dz];
                    if !self.pin {
                        self.anchor = Some(pos);
                    } else if step > MIN_STEP_M {
                        physics.position = HavokPosition(a.0, pos.1, a.2, pos.3);
                        physics.chr_proxy_pos_update_requested = true;
                    }
                    step
                }
            }
        };
        *VIRTUAL.lock().unwrap_or_else(|p| p.into_inner()) = Some(self.virt);
        self.track_dodge(anim, step, before, frame);
    }

    fn track_dodge(&mut self, anim: i32, step: f32, before: [f32; 3], frame: u64) {
        let dodging = is_dodge_anim(anim);
        if self.dodge.as_ref().is_some_and(|d| !dodging || d.anim != anim) {
            let d = self.dodge.take().unwrap();
            let dist = (self.virt[0] - d.start[0]).hypot(self.virt[2] - d.start[2]);
            crate::info!(
                "park",
                "frame={frame} dodge anim={} frames={} virtual={dist:.2} m max_drift={:.3} m (pin={})",
                d.anim,
                d.frames,
                d.max_drift,
                self.pin as u8
            );
        }
        if dodging {
            let d = self.dodge.get_or_insert(Dodge { anim, frames: 0, start: before, max_drift: 0.0 });
            d.frames += 1;
            d.max_drift = d.max_drift.max(step);
        }
    }
}
