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
//! **Weapon (P5 step 2, protocol v10):** InputState.er_weapon (the ER weapon Skyrim mapped from the weapon in its right hand, with
//! an upgrade level) is written into the right-hand slot the stance uses, in both ChrAsm copies (probe 2026-10-05: the moveset and
//! attack.rs's attack rating follow the id; no equip call needed, ER is hidden). Never mid-attack or mid-dodge. The save's own ids
//! come back when the bridge lets go.

use std::sync::atomic::Ordering;

use eldenring::cs::{ChrAsm, ChrAsmArmStyle, ChrAsmEquipmentSlots, PlayerIns};
use skyrimxer_protocol::proto::{InputFlag, InputState, OFF_SLOT_INPUT, Stance};
use skyrimxer_protocol::slot::SlotReader;

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
fn slot_where(ids: &[i32; 22], hand: usize, better: impl Fn(i32, i32) -> bool, ok: impl Fn(i32) -> bool) -> Option<u32> {
    (0..3u32).map(|s| (s, ids[s as usize * 2 + hand])).filter(|&(_, id)| ok(id)).reduce(|a, b| if better(b.1, a.1) { b } else { a }).map(|(s, _)| s)
}

/// `ids` = the save's own equipment ids (never the ones we write: picking by our own write made the slot flip every frame).
fn hold_for(ids: &[i32; 22], stance: u32, saved: Hold) -> Hold {
    let fists_l = slot_where(ids, 0, |_, _| false, |id| id == UNARMED);
    let fists_r = slot_where(ids, 1, |_, _| false, |id| id == UNARMED);
    let weapon = |id: i32| id >= 1_000_000;
    match stance {
        s if s == Stance::OneHanded as u32 => Hold {
            style: ChrAsmArmStyle::OneHanded,
            left: fists_l.unwrap_or(saved.left),
            right: slot_where(ids, 1, |a, b| a < b, weapon).unwrap_or(saved.right),
        },
        s if s == Stance::TwoHanded as u32 => Hold {
            style: ChrAsmArmStyle::RightBothHands,
            left: saved.left,
            right: slot_where(ids, 1, |a, b| a > b, weapon).unwrap_or(saved.right),
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

/// P5 step 2 probe (`weapon=<id>`): puts an ER weapon param id into every right-hand weapon slot (ids 1, 3, 5 that hold a weapon) of
/// both ChrAsm copies, if the EquipParamWeapon row exists. Returns what was replaced.
fn write_weapon(player: &mut PlayerIns, id: i32) -> Result<Vec<i32>, String> {
    use eldenring::cs::{EquipParamWeapon, SoloParamRepository};
    use fromsoftware_shared::FromStatic;
    // SAFETY: main thread; the param repository lives for the whole game.
    let repo = unsafe { SoloParamRepository::instance() }.map_err(|_| "no param repository".to_string())?;
    if repo.get::<EquipParamWeapon>((id / 100 * 100) as u32).is_none() {
        return Err(format!("no EquipParamWeapon row {}", id / 100 * 100));
    }
    // SAFETY: PlayerGameData lives as long as the player; main thread.
    let saved = unsafe { player.player_game_data.as_mut() };
    let mut old = Vec::new();
    for asm in [&mut *player.chr_asm, &mut saved.equipment.chr_asm] {
        for slot in [1usize, 3, 5] {
            if asm.equipment_param_ids[slot] >= 1_000_000 {
                old.push(asm.equipment_param_ids[slot]);
                asm.equipment_param_ids[slot] = id;
            }
        }
    }
    Ok(old)
}

/// Puts `id` into right-hand slot `slot` (0..2) of both ChrAsm copies.
fn write_right(player: &mut PlayerIns, slot: u32, id: i32) {
    // SAFETY: PlayerGameData lives as long as the player; main thread.
    let saved = unsafe { player.player_game_data.as_mut() };
    for asm in [&mut *player.chr_asm, &mut saved.equipment.chr_asm] {
        asm.equipment_param_ids[slot as usize * 2 + 1] = id;
    }
}

/// True if ER has the EquipParamWeapon row (base id) and the ReinforceParamWeapon row for the level.
fn weapon_exists(id: i32) -> bool {
    use eldenring::cs::{EquipParamWeapon, ReinforceParamWeapon, SoloParamRepository};
    use fromsoftware_shared::FromStatic;
    // SAFETY: main thread; the param repository lives for the whole game.
    let Ok(repo) = (unsafe { SoloParamRepository::instance() }) else { return false };
    let Some(w) = repo.get::<EquipParamWeapon>((id / 100 * 100) as u32) else { return false };
    repo.get::<ReinforceParamWeapon>(w.reinforce_type_id() as u32 + (id % 100) as u32).is_some()
}

/// Equips owned ammo the way the game's equip does (bows probe 2026-10-05): arrows (param 50/51xxxxxx) into Arrow1 (slot 6), bolts
/// (52/53xxxxxx) into Bolt1 (slot 7). Four fields: param id + gaitem handle in both ChrAsm copies, the inventory index
/// (`equipment_item_idx_list`: key-items capacity + position in the normal list; matched 4/4 equipped items), and the equip entry
/// (ItemId). ER only fires ammo it owns, so the entry must exist. Returns (slot, inventory position, quantity).
fn equip_ammo(player: &mut PlayerIns, param: u32) -> Result<(usize, usize, u32), String> {
    // SAFETY: PlayerGameData lives as long as the player; main thread.
    let pgd = unsafe { player.player_game_data.as_mut() };
    let inv = &pgd.equipment.equip_inventory_data.items_data;
    let offset = inv.key_items_capacity as usize;
    let (pos, handle, item, qty) = inv
        .normal_entries()
        .iter()
        .enumerate()
        .find_map(|(i, e)| {
            let e = e.as_option()?;
            (e.item_id.param_id() == param).then(|| {
                // SAFETY: GaitemHandle / ItemId are u32 newtypes (eldenring-rs bitfields).
                let h = unsafe { *(&e.gaitem_handle as *const _ as *const u32) };
                let id = unsafe { *(&e.item_id as *const _ as *const u32) };
                (i, h, id, e.quantity)
            })
        })
        .ok_or_else(|| format!("no ammo {param} in the inventory"))?;
    let slot = if matches!(param / 1_000_000, 52 | 53) { 7usize } else { 6 };
    for asm in [&mut *player.chr_asm, &mut pgd.equipment.chr_asm] {
        asm.equipment_param_ids[slot] = param as i32;
        // SAFETY: as above.
        unsafe { *(&mut asm.gaitem_handles[slot] as *mut _ as *mut u32) = handle };
    }
    pgd.equipment.equipment_item_idx_list[slot] = (offset + pos) as u32;
    let entry = if slot == 6 { &mut pgd.equipment.equipment_entries.arrow_primary } else { &mut pgd.equipment.equipment_entries.bolt_primary };
    // SAFETY: OptionalItemId is a u32 newtype.
    unsafe { *(entry as *mut _ as *mut u32) = item };
    Ok((slot, pos, qty))
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
    /// The save's right-hand weapon ids (slots 0..2), taken at spawn, restored when the bridge lets go.
    saved_right: Option<[i32; 3]>,
    /// The save's whole equipment id list at spawn (slot choice by weapon weight uses it, not our writes).
    saved_ids: Option<[i32; 22]>,
    /// (slot, id) we wrote last; None = the save's ids are in place.
    weapon_applied: Option<(u32, i32)>,
    /// An er_weapon ER doesn't have (logged once).
    missing: i32,
    /// The save's Arrow1/Bolt1 equip (restored when we stop writing ammo) and the ammo we equipped last.
    saved_ammo: Option<[AmmoSlot; 2]>,
    ammo_applied: Option<u32>,
    /// ER's count of the ammo we equipped, kept there while we use it (each ER shot uses one; Skyrim's arrows are the real resource).
    ammo_keep: u32,
}

/// One ammo slot's four equip fields (see `equip_ammo`).
#[derive(Clone, Copy, Debug)]
struct AmmoSlot {
    param: [i32; 2],
    gaitem: [u32; 2],
    idx: u32,
    entry: u32,
}

fn read_ammo(player: &mut PlayerIns, slot: usize) -> AmmoSlot {
    // SAFETY: PlayerGameData lives as long as the player; main thread. GaitemHandle / OptionalItemId are u32 newtypes.
    let pgd = unsafe { player.player_game_data.as_mut() };
    let live = &player.chr_asm;
    let saved = &pgd.equipment.chr_asm;
    let entry = if slot == 6 { &pgd.equipment.equipment_entries.arrow_primary } else { &pgd.equipment.equipment_entries.bolt_primary };
    unsafe {
        AmmoSlot {
            param: [live.equipment_param_ids[slot], saved.equipment_param_ids[slot]],
            gaitem: [*(&live.gaitem_handles[slot] as *const _ as *const u32), *(&saved.gaitem_handles[slot] as *const _ as *const u32)],
            idx: pgd.equipment.equipment_item_idx_list[slot],
            entry: *(entry as *const _ as *const u32),
        }
    }
}

fn write_ammo_slot(player: &mut PlayerIns, slot: usize, a: AmmoSlot) {
    // SAFETY: as read_ammo.
    let pgd = unsafe { player.player_game_data.as_mut() };
    for (k, asm) in [&mut *player.chr_asm, &mut pgd.equipment.chr_asm].into_iter().enumerate() {
        asm.equipment_param_ids[slot] = a.param[k];
        unsafe { *(&mut asm.gaitem_handles[slot] as *mut _ as *mut u32) = a.gaitem[k] };
    }
    pgd.equipment.equipment_item_idx_list[slot] = a.idx;
    let entry = if slot == 6 { &mut pgd.equipment.equipment_entries.arrow_primary } else { &mut pgd.equipment.equipment_entries.bolt_primary };
    unsafe { *(entry as *mut _ as *mut u32) = a.entry };
}

/// First owned ammo of a family (arrows 50/51xxxxxx or bolts 52/53xxxxxx).
fn any_owned_ammo(player: &PlayerIns, bolts: bool) -> Option<u32> {
    // SAFETY: as read_ammo.
    let pgd = unsafe { player.player_game_data.as_ref() };
    pgd.equipment.equip_inventory_data.items_data.items().map(|e| e.item_id.param_id()).find(|id| {
        let family = id / 1_000_000;
        if bolts { matches!(family, 52 | 53) } else { matches!(family, 50 | 51) }
    })
}

impl StanceSync {
    pub fn new() -> Self {
        Self { reader: None, last_input: None, spawned_at: None, saved: None, applied: None, override_done: false, last_line: String::new(), saved_right: None, saved_ids: None, weapon_applied: None, missing: 0, saved_ammo: None, ammo_applied: None, ammo_keep: 0 }
    }

    pub fn run(&mut self) {
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.spawned_at = None;
            self.saved = None;
            self.applied = None;
            self.saved_right = None;
            self.saved_ids = None;
            self.saved_ammo = None;
            self.ammo_applied = None;
            self.weapon_applied = None;
            return;
        };
        let saved_ids = *self.saved_ids.get_or_insert(player.chr_asm.equipment_param_ids);
        if self.saved_ammo.is_none() {
            self.saved_ammo = Some([read_ammo(player, 6), read_ammo(player, 7)]);
        }
        let saved_right = *self.saved_right.get_or_insert_with(|| {
            let ids = &player.chr_asm.equipment_param_ids;
            [ids[1], ids[3], ids[5]]
        });
        let spawned = *self.spawned_at.get_or_insert(frame);
        let saved = *self.saved.get_or_insert_with(|| hold_of(&player.chr_asm));
        let line = describe(&player.chr_asm);
        if line != self.last_line {
            crate::info!("stance", "frame={frame} ER {line}");
            self.last_line = line;
        }
        let weapon = config::get().weapon;
        if weapon != 0 && !self.override_done && frame - spawned >= WRITE_AFTER_FRAMES {
            self.override_done = true;
            let ammo = config::get().ammo;
            if ammo != 0 {
                match equip_ammo(player, ammo as u32) {
                    Ok((slot, pos, qty)) => crate::info!("stance", "frame={frame} AMMO PROBE: equipped {ammo} in slot {slot} (inventory normal[{pos}], {qty} owned)"),
                    Err(e) => crate::warn!("stance", "frame={frame} AMMO PROBE: {e}"),
                }
            }
            match write_weapon(player, weapon) {
                Ok(old) => crate::info!("stance", "frame={frame} WEAPON PROBE: right-hand slots {old:?} → {weapon}; now {}", describe(&player.chr_asm)),
                Err(e) => crate::warn!("stance", "frame={frame} WEAPON PROBE {weapon} not written: {e}"),
            }
        }
        if let Some(style) = override_style() {
            if !self.override_done && frame - spawned >= WRITE_AFTER_FRAMES {
                self.override_done = true;
                let mut hold = Hold { style, ..saved };
                if config::get().stance == "fists" {
                    hold = hold_for(&saved_ids, Stance::Unarmed as u32, saved);
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
        // Stale input while connected = Skyrim is paused in a menu: keep its stance/weapon/ammo (putting ER's own back and re-equipping
        // after every menu made the next bow shot fail, 2026-10-05). ER's own come back on F10 off or when Skyrim goes away.
        let input = self.last_input.filter(|i| connected && i.flags & BRIDGE_ON != 0);
        let anim = game::snapshot(player).anim_id;
        if park::is_dodge_anim(anim) || crate::attack::kind_of(anim) != skyrimxer_protocol::proto::AttackKind::None {
            return; // never switch mid-dodge or mid-swing
        }
        let want = input.map(|i| hold_for(&saved_ids, i.stance, saved));
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
        // The weapon Skyrim holds (v10), into the right-hand slot this stance uses.
        let right = self.applied.map(|h| h.right);
        let want_weapon = match (input, right) {
            (Some(i), Some(slot)) if i.er_weapon != 0 && i.stance != Stance::Unarmed as u32 => {
                if weapon_exists(i.er_weapon) {
                    Some((slot, i.er_weapon))
                } else {
                    if self.missing != i.er_weapon {
                        self.missing = i.er_weapon;
                        crate::warn!("stance", "frame={frame} ER has no weapon {} (EquipParamWeapon/ReinforceParamWeapon row): keeping its own", i.er_weapon);
                    }
                    None
                }
            }
            _ => None,
        };
        if want_weapon != self.weapon_applied {
            // Put the save's id back where we wrote before, then write the new one.
            if let Some((slot, _)) = self.weapon_applied {
                write_right(player, slot, saved_right[slot as usize]);
            }
            if let Some((slot, id)) = want_weapon {
                write_right(player, slot, id);
                crate::info!("stance", "frame={frame} weapon: Skyrim's → ER {} (+{}) in right slot {slot}", id / 100 * 100, id % 100);
            } else {
                crate::info!("stance", "frame={frame} weapon: ER's own back {saved_right:?}");
            }
            self.weapon_applied = want_weapon;
        }
        // Bows/crossbows (v12): ER fires only ammo it owns, equipped the full way (equip_ammo). Prefer the mapped id, else any owned of
        // that family; the save's own ammo comes back when the weapon does.
        let want_ammo = match (input, want_weapon) {
            (Some(i), Some(_)) if i.er_ammo != 0 => {
                let bolts = matches!(i.er_ammo / 1_000_000, 52 | 53);
                let owned = |id: u32| {
                    // SAFETY: as read_ammo.
                    let pgd = unsafe { player.player_game_data.as_ref() };
                    pgd.equipment.equip_inventory_data.items_data.items().any(|e| e.item_id.param_id() == id)
                };
                if owned(i.er_ammo as u32) { Some(i.er_ammo as u32) } else { any_owned_ammo(player, bolts) }
            }
            _ => None,
        };
        if want_ammo != self.ammo_applied {
            if let (Some(_), Some(saved)) = (self.ammo_applied, self.saved_ammo) {
                write_ammo_slot(player, 6, saved[0]);
                write_ammo_slot(player, 7, saved[1]);
            }
            match want_ammo.map(|id| (id, equip_ammo(player, id))) {
                Some((id, Ok((slot, pos, qty)))) => {
                    self.ammo_keep = qty.max(1);
                    crate::info!("stance", "frame={frame} ammo: ER {id} equipped in slot {slot} (inventory [{pos}], {qty} owned, kept at that)");
                }
                Some((id, Err(e))) => crate::warn!("stance", "frame={frame} ammo {id}: {e}"),
                None if self.ammo_applied.is_some() => crate::info!("stance", "frame={frame} ammo: the save's own back"),
                None => {
                    if input.is_some_and(|i| i.er_ammo != 0) {
                        crate::warn!("stance", "frame={frame} ammo: the ER character owns no arrows/bolts for this bow: it can't shoot");
                    }
                }
            }
            self.ammo_applied = want_ammo;
        }
        if let Some(id) = self.ammo_applied {
            // SAFETY: as read_ammo.
            let pgd = unsafe { player.player_game_data.as_mut() };
            if let Some(e) = pgd.equipment.equip_inventory_data.items_data.items_mut().find(|e| e.item_id.param_id() == id)
                && e.quantity < self.ammo_keep
            {
                e.quantity = self.ammo_keep;
            }
        }
    }
}
