//! Skyrim ⇄ ER over the protocol slots (P3 step 4, v3 since P4 step 3). Main thread only (task callbacks); the link's lock is never taken here.
//! - [`DodgeFromSkyrim`] (in `actions::inject_group()`): Skyrim's held Dodge → the virtual Backstep key, the same way the step-2
//!   self-test presses it (hold Backstep, BackstepTapped on the press frame only). ER's own gating decides what happens
//!   (tap = backstep/roll, hold = dash). Stale or disconnected input = nothing held (fail-safe).
//!   Skyrim's movement keys (`move_x/move_y`) → the virtual move stick, so Dodge + direction = roll (P3 step 5). Only while Dodge
//!   is held and STICK_AFTER_RELEASE frames after (P4 step 1): Skyrim walks by itself, so the hidden character stays parked.
//! - [`publish_state`] (ChrIns_PostPhysics in world, FrameBegin otherwise): the player snapshot → PlayerState every frame.
//! - [`sample_coords`] (ChrIns_PostPhysics): bounded `[coords]` position/yaw samples while moving (coordinate test).

use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use eldenring::cs::UserInputKey;
use skyrimxer_protocol::now_ms;
use skyrimxer_protocol::proto::{Button, InputFlag, InputState, OFF_SLOT_INPUT, OFF_SLOT_PLAYER, PlayerFlag, PlayerState};
use skyrimxer_protocol::slot::{SlotReader, SlotWriter, fresh};

use crate::{actions, bridge, combat, game, pad};

const DODGE: u32 = 1 << Button::Dodge as u32;
const IN_COMBAT: u32 = 1 << InputFlag::InCombat as u32;
const BRIDGE_ON: u32 = 1 << InputFlag::BridgeOn as u32;
/// Frames the move stick stays forwarded after Dodge is released (ER picks the roll direction on the release frame or just after).
const STICK_AFTER_RELEASE: u32 = 10;
/// A Dodge held longer than this is a sprint, not a dodge: the stick is dropped so ER doesn't dash the hidden character away (Skyrim
/// lets its own sprint through after the same time, hooks/SprintSwallow.cpp). Taps are 5-15 frames.
const DASH_AFTER: u32 = 20;

/// True while Skyrim input drives the ER character (Dodge held or the move stick forwarded); park.rs keeps the parking spot still then.
pub static DRIVING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

fn flag(f: PlayerFlag) -> u32 {
    1 << f as u32
}

pub struct DodgeFromSkyrim {
    reader: Option<SlotReader<InputState>>,
    /// Last good read: a failed read (every try torn, writer pre-empted mid-write) must not release a held Dodge for a frame.
    last_input: Option<InputState>,
    fresh: bool,
    held: bool,
    held_frames: u32,
    /// Frames since Dodge was released (0 while held, saturates).
    since_release: u32,
    /// The current/last press was held past DASH_AFTER (a sprint): no stick for it, not even after the release.
    long_press: bool,
    /// Move stick we hold (x right, y forward); (0, 0) = not holding, so a real keyboard still works with `-ErVisible`.
    moving: (f32, f32),
}

impl DodgeFromSkyrim {
    pub fn new() -> Self {
        Self { reader: None, last_input: None, fresh: false, held: false, held_frames: 0, since_release: u32::MAX, long_press: false, moving: (0.0, 0.0) }
    }

    pub fn run(&mut self) {
        let Some(shared) = bridge::shared() else { return };
        if self.reader.is_none() {
            self.reader = shared.region().map(|r| SlotReader::new(r, OFF_SLOT_INPUT));
        }
        let Some(reader) = &self.reader else { return };
        let now = now_ms();
        self.last_input = reader.read().or(self.last_input);
        let input = self.last_input;
        let connected = shared.connected();
        let is_fresh = connected && input.is_some_and(|i| fresh(i.time_ms, now));
        if is_fresh != self.fresh {
            self.fresh = is_fresh;
            if is_fresh {
                crate::info!("input", "InputState fresh: Skyrim input is live");
            } else {
                let why = match input {
                    _ if !connected => "link not connected".to_string(),
                    None => "never written".to_string(),
                    Some(i) => format!("last write {} ms ago", now.saturating_sub(i.time_ms)),
                };
                crate::warn!("input", "InputState stale ({why}); injecting nothing");
            }
        }

        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        // Skyrim's combat state → ER's (applied in ChrIns_AILogic by combat::MirrorCombat). Bridge off or stale = ER decides itself.
        let combat = match input {
            Some(i) if is_fresh && i.flags & BRIDGE_ON != 0 => Some(i.flags & IN_COMBAT != 0),
            _ => None,
        };
        combat::want(combat);
        // SAFETY: task callbacks run on the game's main thread.
        let player = unsafe { game::main_player() };
        let held = is_fresh && player.is_some() && input.is_some_and(|i| i.buttons & DODGE != 0);
        self.since_release = if held { 0 } else { self.since_release.saturating_add(1) };
        if held && !self.held {
            self.long_press = false;
        } else if held && self.held_frames >= DASH_AFTER && !self.long_press {
            self.long_press = true;
            crate::info!("input", "frame={frame} Dodge held {} frames: sprint, not a dodge (stick dropped, ER character stays)", self.held_frames);
        }
        let stick_window = self.since_release <= STICK_AFTER_RELEASE && !self.long_press;
        let moving = match input {
            Some(i) if is_fresh && player.is_some() && stick_window => (i.move_x.clamp(-1.0, 1.0), i.move_y.clamp(-1.0, 1.0)),
            _ => (0.0, 0.0),
        };
        if moving != self.moving {
            let seq = input.map_or(0, |i| i.seq);
            let why = if stick_window { "dodge window" } else if self.long_press { "sprint hold" } else { "dodge window over" };
            crate::info!("input", "frame={frame} move x={:.2} y={:.2} (InputState seq={seq}, {why})", moving.0, moving.1);
            // SAFETY: main thread. The edge to (0, 0) writes the release once.
            unsafe { pad::set_move(moving.0, moving.1) };
            self.moving = moving;
            MOVE.store(u64::from(moving.0.to_bits()) << 32 | u64::from(moving.1.to_bits()), Ordering::Relaxed);
        } else if moving != (0.0, 0.0) {
            // SAFETY: main thread. PadStep re-copies the device data each frame, so a held stick is written every frame.
            unsafe { pad::set_move(moving.0, moving.1) };
        }
        DRIVING.store(held || moving != (0.0, 0.0), Ordering::Relaxed);
        // SAFETY (all pad calls): main thread.
        if held && !self.held {
            let (i, s) = (input.unwrap(), game::snapshot(player.unwrap()));
            crate::info!(
                "input",
                "frame={frame} Dodge down (InputState frame={} seq={} age={}ms, move x={:.2} y={:.2}) → Backstep; before: stamina={} anim={}",
                i.frame,
                i.seq,
                now.saturating_sub(i.time_ms),
                moving.0,
                moving.1,
                s.stamina,
                s.anim_id
            );
            unsafe {
                pad::set_digital(UserInputKey::Backstep, true);
                pad::set_digital(UserInputKey::BackstepTapped, true);
            }
            actions::watch(frame);
            self.held_frames = 1;
        } else if held {
            unsafe {
                pad::set_digital(UserInputKey::Backstep, true);
                pad::set_digital(UserInputKey::BackstepTapped, false);
            }
            self.held_frames += 1;
        } else if self.held {
            let why = if is_fresh { "released" } else { "input went stale" };
            crate::info!("input", "frame={frame} Dodge up after {} frames ({why})", self.held_frames);
            unsafe {
                pad::set_digital(UserInputKey::Backstep, false);
                pad::set_digital(UserInputKey::BackstepTapped, false);
            }
        }
        self.held = held;
    }
}

/// Move stick held for Skyrim right now (x bits << 32 | y bits), for the `[coords]` lines.
static MOVE: AtomicU64 = AtomicU64::new(0);
/// Coordinate test probe: one sample every COORDS_EVERY frames while moving, at most COORDS_MAX_LINES per session.
const COORDS_EVERY: u64 = 6;
const COORDS_MAX_LINES: u32 = 1500;
static COORDS_LINES: AtomicU32 = AtomicU32::new(0);
static COORDS_LAST: Mutex<[f32; 3]> = Mutex::new([0.0; 3]);

/// Coordinate test (docs/research/coordinates.md): position (Y-up, metres) and yaw every COORDS_EVERY frames while the player moved
/// more than 1 cm since the last sample or Skyrim holds a direction. Bounded.
pub fn sample_coords() {
    let frame = bridge::FRAMES.load(Ordering::Relaxed);
    if frame % COORDS_EVERY != 0 || COORDS_LINES.load(Ordering::Relaxed) >= COORDS_MAX_LINES {
        return;
    }
    // SAFETY: main thread (task callback).
    let Some(player) = (unsafe { game::main_player() }) else { return };
    let s = game::snapshot(player);
    let bits = MOVE.load(Ordering::Relaxed);
    let (mx, my) = (f32::from_bits((bits >> 32) as u32), f32::from_bits(bits as u32));
    let mut last = COORDS_LAST.lock().unwrap_or_else(|p| p.into_inner());
    let dist = s.pos.iter().zip(last.iter()).map(|(a, b)| (a - b) * (a - b)).sum::<f32>().sqrt();
    if mx == 0.0 && my == 0.0 && dist <= 0.01 {
        return;
    }
    *last = s.pos;
    COORDS_LINES.fetch_add(1, Ordering::Relaxed);
    crate::info!(
        "coords",
        "frame={frame} pos=({:.3},{:.3},{:.3}) yaw={:.4} move={mx},{my} anim={}",
        s.pos[0],
        s.pos[1],
        s.pos[2],
        s.yaw,
        s.anim_id
    );
}

/// One writer shared by the two publish points (both on the main thread).
static WRITER: Mutex<Option<SlotWriter<PlayerState>>> = Mutex::new(None);

/// Writes PlayerState. Called from ChrIns_PostPhysics (`from_frame_begin = false`: in-world frames, after physics) and from FrameBegin
/// (`true`: only while there is no player, because the ChrIns groups barely run at the title screen and Skyrim would see it go stale).
pub fn publish_state(from_frame_begin: bool) {
    // SAFETY: main thread (task callbacks).
    let player = unsafe { game::main_player() };
    if from_frame_begin == player.is_some() {
        return;
    }
    let mut writer = WRITER.lock().unwrap_or_else(|p| p.into_inner());
    if writer.is_none() {
        *writer = bridge::shared().and_then(|s| s.region()).map(|r| SlotWriter::new(r, OFF_SLOT_PLAYER));
    }
    let Some(writer) = writer.as_mut() else { return };
    let mut state = PlayerState { frame: bridge::FRAMES.load(Ordering::Relaxed), time_ms: now_ms(), ..Default::default() };
    // No player (title screen / loading) = flags 0: Skyrim sees "not in world", not stale.
    if let Some(player) = player {
        let s = game::snapshot(player);
        state.flags = flag(PlayerFlag::InWorld)
            | if s.iframe { flag(PlayerFlag::IFrame) } else { 0 }
            | if s.dodging { flag(PlayerFlag::Dodging) } else { 0 }
            | if s.hyperarmor { flag(PlayerFlag::HyperArmor) } else { 0 }
            | if s.poise_broken { flag(PlayerFlag::PoiseBroken) } else { 0 }
            | if combat::in_combat(player) { flag(PlayerFlag::InCombat) } else { 0 }
            | if s.move_cancel { flag(PlayerFlag::MoveCancel) } else { 0 };
        state.hp = s.hp;
        state.max_hp = s.max_hp;
        state.fp = s.fp;
        state.max_fp = s.max_fp;
        state.stamina = s.stamina;
        state.max_stamina = s.max_stamina;
        state.anim_id = s.anim_id;
        state.block_id = s.block_id;
        state.poise = s.poise;
        state.poise_max = s.poise_max;
        // Pinned character (park.rs): Skyrim follows where it would be, not where it is.
        state.pos = crate::park::virtual_pos().unwrap_or(s.pos);
        state.yaw = s.yaw;
    }
    writer.write(&state);
}
