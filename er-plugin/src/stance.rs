//! ER's weapon stance follows Skyrim's (InputState.stance). The streamed pose copies ER's stance, so an ER character two-handing a
//! weapon backstepped and rolled "holding something" while the Skyrim player was unarmed (2026-10-05 playtest).
//!
//! ER keeps the stance in ChrAsm (eldenring-rs player_game_data.rs): the live one on PlayerIns (`chr_asm`) and the saved one in
//! PlayerGameData. Writing both switches the animation set (probe 2026-10-05, ER backstep self-test):
//! - the save's RightBothHands (colossal sword in R1) backsteps as 12027010;
//! - OneHanded / EmptyHanded as 2027010 (the weapon is still in the right hand);
//! - OneHanded with both hands' Unarmed (110000) slots selected as 27010, the plain set.
//!
//! Sync (ChrIns_PostPhysics): Skyrim Unarmed → fists, OneHanded → the lightest right-hand weapon, TwoHanded → the heaviest, two-handed.
//! Never mid-dodge. Bridge off / Skyrim stale / leaving the world → the save's own stance back. `stance=` in skyrimxer_er.cfg
//! (dev.ps1 -ErStance empty|one|right2|left2|fists) overrides the sync once after spawn (research).
//! Weapon *types* aren't matched yet, only how they're held (real weapon mapping: P5).

use std::sync::atomic::Ordering;

use eldenring::cs::{ChrAsm, ChrAsmArmStyle, ChrAsmEquipmentSlots, PlayerIns};
use skyrimxer_protocol::now_ms;
use skyrimxer_protocol::proto::{InputFlag, InputState, OFF_SLOT_INPUT, Stance};
use skyrimxer_protocol::slot::{SlotReader, fresh};

use crate::{bridge, config, game, park};

const WRITE_AFTER_FRAMES: u64 = 300;
const UNARMED: i32 = 110_000;
const BRIDGE_ON: u32 = 1 << InputFlag::BridgeOn as u32;

fn describe(asm: &ChrAsm) -> String {
    let e = &asm.equipment;
    let ids = &asm.equipment_param_ids;
    format!(
        "arm_style={:?} slots L={} R={} | weapons L1={} R1={} L2={} R2={} L3={} R3={}",
        e.arm_style, e.selected_slots.left_weapon_slot, e.selected_slots.right_weapon_slot, ids[0], ids[1], ids[2], ids[3], ids[4], ids[5]
    )
}

fn override_style() -> Option<ChrAsmArmStyle> {
    match config::get().stance.as_str() {
        "empty" => Some(ChrAsmArmStyle::EmptyHanded),
        "fists" | "one" => Some(ChrAsmArmStyle::OneHanded),
        "left2" => Some(ChrAsmArmStyle::LeftBothHands),
        "right2" => Some(ChrAsmArmStyle::RightBothHands),
        _ => None,
    }
}

/// Stance + selected weapon slots (what we write).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct Hold {
    style: ChrAsmArmStyle,
    left: u32,
    right: u32,
}

fn hold_of(asm: &ChrAsm) -> Hold {
    let e = &asm.equipment;
    Hold { style: e.arm_style, left: e.selected_slots.left_weapon_slot, right: e.selected_slots.right_weapon_slot }
}

/// Slot (0..2) of a hand whose weapon id matches `pick` best; `hand` 0 = left (ids 0,2,4), 1 = right (ids 1,3,5).
fn slot_where(asm: &ChrAsm, hand: usize, better: impl Fn(i32, i32) -> bool, ok: impl Fn(i32) -> bool) -> Option<u32> {
    let ids = &asm.equipment_param_ids;
    (0..3u32).map(|s| (s, ids[s as usize * 2 + hand])).filter(|&(_, id)| ok(id)).reduce(|a, b| if better(b.1, a.1) { b } else { a }).map(|(s, _)| s)
}

fn hold_for(asm: &ChrAsm, stance: u32, saved: Hold) -> Hold {
    let fists_l = slot_where(asm, 0, |_, _| false, |id| id == UNARMED);
    let fists_r = slot_where(asm, 1, |_, _| false, |id| id == UNARMED);
    let weapon = |id: i32| id >= 1_000_000;
    match stance {
        s if s == Stance::OneHanded as u32 => Hold {
            style: ChrAsmArmStyle::OneHanded,
            left: fists_l.unwrap_or(saved.left),
            right: slot_where(asm, 1, |a, b| a < b, weapon).unwrap_or(saved.right),
        },
        s if s == Stance::TwoHanded as u32 => Hold {
            style: ChrAsmArmStyle::RightBothHands,
            left: saved.left,
            right: slot_where(asm, 1, |a, b| a > b, weapon).unwrap_or(saved.right),
        },
        // Unarmed: fists in both hands when the character has Unarmed slots; else at least not two-handing.
        _ => match (fists_l, fists_r) {
            (Some(l), Some(r)) => Hold { style: ChrAsmArmStyle::OneHanded, left: l, right: r },
            _ => Hold { style: ChrAsmArmStyle::EmptyHanded, ..saved },
        },
    }
}

fn write(player: &mut PlayerIns, hold: Hold) {
    // SAFETY: PlayerGameData lives as long as the player; main thread (task callback).
    let saved = unsafe { player.player_game_data.as_mut() };
    for asm in [&mut *player.chr_asm, &mut saved.equipment.chr_asm] {
        asm.equipment.arm_style = hold.style;
        asm.equipment.selected_slots =
            ChrAsmEquipmentSlots { left_weapon_slot: hold.left, right_weapon_slot: hold.right, ..asm.equipment.selected_slots };
    }
}

pub struct StanceSync {
    reader: Option<SlotReader<InputState>>,
    last_input: Option<InputState>,
    spawned_at: Option<u64>,
    /// The save's own stance, taken at spawn (restored when the bridge lets go).
    saved: Option<Hold>,
    /// What we wrote last (None = the save's stance is in place).
    applied: Option<Hold>,
    override_done: bool,
    last_line: String,
}

impl StanceSync {
    pub fn new() -> Self {
        Self { reader: None, last_input: None, spawned_at: None, saved: None, applied: None, override_done: false, last_line: String::new() }
    }

    pub fn run(&mut self) {
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.spawned_at = None;
            self.saved = None;
            self.applied = None;
            return;
        };
        let spawned = *self.spawned_at.get_or_insert(frame);
        let saved = *self.saved.get_or_insert_with(|| hold_of(&player.chr_asm));
        let line = describe(&player.chr_asm);
        if line != self.last_line {
            crate::info!("stance", "frame={frame} ER {line}");
            self.last_line = line;
        }
        if let Some(style) = override_style() {
            if !self.override_done && frame - spawned >= WRITE_AFTER_FRAMES {
                self.override_done = true;
                let mut hold = Hold { style, ..saved };
                if config::get().stance == "fists" {
                    hold = hold_for(&player.chr_asm, Stance::Unarmed as u32, saved);
                }
                write(player, hold);
                crate::info!("stance", "frame={frame} OVERRIDE (stance={}) → {hold:?}", config::get().stance);
            }
            return;
        }

        if self.reader.is_none() {
            self.reader = bridge::shared().and_then(|s| s.region()).map(|r| SlotReader::new(r, OFF_SLOT_INPUT));
        }
        // A torn read keeps the last good copy (remote.rs does the same).
        self.last_input = self.reader.as_ref().and_then(|r| r.read()).or(self.last_input);
        let connected = bridge::shared().is_some_and(|s| s.connected());
        let input = self.last_input.filter(|i| connected && fresh(i.time_ms, now_ms()) && i.flags & BRIDGE_ON != 0);
        let anim = game::snapshot(player).anim_id;
        if park::is_dodge_anim(anim) {
            return; // never switch mid-dodge
        }
        let want = input.map(|i| hold_for(&player.chr_asm, i.stance, saved));
        match (want, self.applied) {
            (Some(hold), applied) if applied != Some(hold) => {
                write(player, hold);
                self.applied = Some(hold);
                let weapon = player.chr_asm.equipment_param_ids[hold.right as usize * 2 + 1];
                crate::info!("stance", "frame={frame} Skyrim stance {} → ER {hold:?} (right weapon {weapon})", input.map_or(0, |i| i.stance));
            }
            (None, Some(_)) => {
                write(player, saved);
                self.applied = None;
                crate::info!("stance", "frame={frame} bridge off / Skyrim gone → ER back to the save's stance {saved:?}");
            }
            _ => {}
        }
    }
}
