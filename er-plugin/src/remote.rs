//! Skyrim ⇄ ER over the protocol v2 slots (P3 step 4). Main thread only (task callbacks); the link's lock is never taken here.
//! - [`DodgeFromSkyrim`] (in `actions::inject_group()`): Skyrim's held Dodge → the virtual Backstep key, the same way the step-2
//!   self-test presses it (hold Backstep, BackstepTapped on the press frame only). ER's own gating decides what happens
//!   (tap = backstep/roll, hold = dash). Stale or disconnected input = nothing held (fail-safe).
//! - [`publish_state`] (ChrIns_PostPhysics in world, FrameBegin otherwise): the player snapshot → PlayerState every frame.

use std::sync::Mutex;
use std::sync::atomic::Ordering;

use eldenring::cs::UserInputKey;
use skyrimxer_protocol::now_ms;
use skyrimxer_protocol::proto::{Button, InputState, OFF_SLOT_INPUT, OFF_SLOT_PLAYER, PlayerFlag, PlayerState};
use skyrimxer_protocol::slot::{SlotReader, SlotWriter, fresh};

use crate::{actions, bridge, game, pad};

const DODGE: u32 = 1 << Button::Dodge as u32;

fn flag(f: PlayerFlag) -> u32 {
    1 << f as u32
}

pub struct DodgeFromSkyrim {
    reader: Option<SlotReader<InputState>>,
    fresh: bool,
    held: bool,
    held_frames: u32,
}

impl DodgeFromSkyrim {
    pub fn new() -> Self {
        Self { reader: None, fresh: false, held: false, held_frames: 0 }
    }

    pub fn run(&mut self) {
        let Some(shared) = bridge::shared() else { return };
        if self.reader.is_none() {
            self.reader = shared.region().map(|r| SlotReader::new(r, OFF_SLOT_INPUT));
        }
        let Some(reader) = &self.reader else { return };
        let now = now_ms();
        let input = reader.read();
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
        // SAFETY: task callbacks run on the game's main thread.
        let player = unsafe { game::main_player() };
        let held = is_fresh && player.is_some() && input.is_some_and(|i| i.buttons & DODGE != 0);
        // SAFETY (all pad calls): main thread.
        if held && !self.held {
            let (i, s) = (input.unwrap(), game::snapshot(player.unwrap()));
            crate::info!(
                "input",
                "frame={frame} Dodge down (InputState frame={} seq={} age={}ms) → Backstep; before: stamina={} anim={}",
                i.frame,
                i.seq,
                now.saturating_sub(i.time_ms),
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
            | if s.poise_broken { flag(PlayerFlag::PoiseBroken) } else { 0 };
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
        state.pos = s.pos;
        state.yaw = s.yaw;
    }
    writer.write(&state);
}
