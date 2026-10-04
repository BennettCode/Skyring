//! ER combat state (P4 step 2, docs/research/elden-ring-state.md). Rolls cost no stamina out of combat; the switch is one flag bit
//! ([`in_combat`] / [`set_in_combat`]) that ER recomputes every frame in ChrIns_NaviCache, so a write in ChrIns_AILogic holds.
//! Research tools (dump=1): [`SpEffectWatch`] logs the player's SpEffect list changes (ruled SpEffects out), [`Dump`] writes raw memory
//! snapshots (found the flag). [`CombatWatch`] logs the state's edges in every session. Main thread only (task callbacks).

use std::collections::BTreeSet;
use std::sync::atomic::Ordering;

use eldenring::cs::SpecialEffectEntry;

use crate::{bridge, game};

const MAX_LINES: u32 = 2000;

/// ER's "out of combat" flag (found by the step-2 dump, docs/research/elden-ring-state.md): `CSChrDataModule` byte +0x19a, bit 0x40
/// (eldenring-rs `unk198`). Set = out of combat (dodges cost no stamina). The game clears it when an enemy fights the player and sets
/// it again once the fight is over (e.g. resting at a grace).
const OUT_OF_COMBAT_OFFSET: usize = 0x19a;
const OUT_OF_COMBAT_BIT: u8 = 0x40;

fn out_of_combat_byte(player: &mut eldenring::cs::PlayerIns) -> *mut u8 {
    let data: *mut _ = &mut *player.chr_ins.modules.data;
    // SAFETY: CSChrDataModule is larger than 0x19a bytes (eldenring-rs layout: unk198..unk19c).
    unsafe { data.cast::<u8>().add(OUT_OF_COMBAT_OFFSET) }
}

/// ER's own combat state: true while an enemy fights the player (dodges cost stamina).
pub fn in_combat(player: &mut eldenring::cs::PlayerIns) -> bool {
    // SAFETY: see out_of_combat_byte; main thread.
    unsafe { *out_of_combat_byte(player) & OUT_OF_COMBAT_BIT == 0 }
}

/// Writes ER's combat state. Returns whether the game had a different value right before (it may rewrite the flag every frame).
pub fn set_in_combat(player: &mut eldenring::cs::PlayerIns, on: bool) -> bool {
    let p = out_of_combat_byte(player);
    // SAFETY: see out_of_combat_byte; main thread.
    unsafe {
        let old = *p;
        let new = if on { old & !OUT_OF_COMBAT_BIT } else { old | OUT_OF_COMBAT_BIT };
        *p = new;
        old != new
    }
}

/// `force_combat=on|off` (`tools/dev.ps1 -ErForceCombat`, research/self-test): writes the flag in one task group every frame and logs,
/// every FORCE_REPORT frames, on how many frames the game had changed it back since our last write.
pub struct ForceCombat {
    group: &'static str,
    on: bool,
    frames: u32,
    overridden: u32,
}

const FORCE_REPORT: u32 = 300;

impl ForceCombat {
    pub fn new(group: &'static str, on: bool) -> Self {
        Self { group, on, frames: 0, overridden: 0 }
    }

    pub fn run(&mut self) {
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else { return };
        self.frames += 1;
        if set_in_combat(player, self.on) {
            self.overridden += 1;
        }
        if self.frames == FORCE_REPORT {
            let what = if self.on { "in combat" } else { "calm" };
            crate::info!("combat", "force {what} in {}: the game had changed it back on {}/{} frames", self.group, self.overridden, self.frames);
            self.frames = 0;
            self.overridden = 0;
        }
    }
}

/// Logs ER's combat-state edges (ChrIns_PostPhysics), with stamina, so every log shows when ER thinks the player is fighting.
pub struct CombatWatch {
    last: Option<bool>,
}

impl CombatWatch {
    pub fn new() -> Self {
        Self { last: None }
    }

    pub fn run(&mut self) {
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.last = None;
            return;
        };
        let now = in_combat(player);
        if self.last != Some(now) {
            let s = game::snapshot(player);
            let frame = bridge::FRAMES.load(Ordering::Relaxed);
            crate::info!("combat", "frame={frame} ER combat state: {} (stamina={} anim={})", if now { "IN COMBAT" } else { "calm" }, s.stamina, s.anim_id);
            self.last = Some(now);
        }
    }
}

pub struct SpEffectWatch {
    last: Option<BTreeSet<i32>>,
    lines: u32,
}

/// `id` plus the effect's stamina-related fields when they aren't neutral (rate 1 / point 0).
fn describe(entry: &SpecialEffectEntry) -> String {
    let mut text = entry.param_id.to_string();
    // SAFETY: param_data points into the loaded SpEffectParam table, which lives for the whole game.
    if let Some(p) = entry.param_data.map(|p| unsafe { p.as_ref() }) {
        let mut notes = Vec::new();
        if p.consume_stamina_rate() != 1.0 {
            notes.push(format!("consume_stamina×{}", p.consume_stamina_rate()));
        }
        if p.max_stamina_rate() != 1.0 {
            notes.push(format!("max_stamina×{}", p.max_stamina_rate()));
        }
        if p.change_stamina_rate() != 0.0 || p.change_stamina_point() != 0 {
            notes.push(format!("change_stamina {}/{}", p.change_stamina_rate(), p.change_stamina_point()));
        }
        if p.stamina_recover_change_speed() != 0 {
            notes.push(format!("stamina_recover {}", p.stamina_recover_change_speed()));
        }
        if !notes.is_empty() {
            text.push_str(&format!(" [{}]", notes.join(", ")));
        }
    }
    if entry.duration > 0.0 {
        text.push_str(&format!(" ({:.1}s)", entry.duration));
    }
    text
}

/// Research dump (`dump=1`, `tools/dev.ps1 -ErDump`): every DUMP_EVERY frames, raw bytes of the PlayerIns struct, its PlayerGameData and
/// the typed ChrIns modules go to `logs/combat_dump.bin`, so the combat-state word can be found offline (words that flip exactly when
/// rolls start/stop costing stamina). Record: u64 frame, i32 stamina, i32 anim, then the regions in the order of `combat_dump.txt`.
pub struct Dump {
    file: Option<std::fs::File>,
    records: u32,
    layout: Vec<(&'static str, usize)>,
}

const DUMP_EVERY: u64 = 15;
const DUMP_MAX_RECORDS: u32 = 4000;

/// (name, start, length) of each dumped region. Lengths come from the eldenring-rs types, so they never read past the object.
fn dump_regions(player: &eldenring::cs::PlayerIns) -> Vec<(&'static str, *const u8, usize)> {
    fn r<T>(name: &'static str, v: &T) -> (&'static str, *const u8, usize) {
        (name, (v as *const T).cast(), std::mem::size_of::<T>())
    }
    let m = &player.chr_ins.modules;
    // SAFETY: the main player's PlayerGameData lives as long as the player.
    let game_data = unsafe { player.player_game_data.as_ref() };
    vec![
        r("PlayerIns", player),
        r("PlayerGameData", game_data),
        r("data", &*m.data),
        r("action_flag", &*m.action_flag),
        r("time_act", &*m.time_act),
        r("behavior", &*m.behavior),
        r("super_armor", &*m.super_armor),
        r("toughness", &*m.toughness),
        r("event", &*m.event),
        r("physics", &*m.physics),
        r("action_request", &*m.action_request),
        r("behavior_data", &*m.behavior_data),
    ]
}

impl Dump {
    pub fn new() -> Self {
        Self { file: None, records: 0, layout: Vec::new() }
    }

    pub fn run(&mut self) {
        use std::io::Write;
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        if self.records >= DUMP_MAX_RECORDS || frame % DUMP_EVERY != 0 {
            return;
        }
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else { return };
        let regions = dump_regions(player);
        if self.file.is_none() {
            let Some(dir) = crate::log::LOG_DIR.get() else { return };
            self.layout = regions.iter().map(|&(n, _, len)| (n, len)).collect();
            let text: Vec<String> = self.layout.iter().map(|(n, len)| format!("{n} {len}")).collect();
            if let Err(e) = std::fs::write(dir.join("combat_dump.txt"), text.join("\n") + "\n") {
                crate::error!("probe", "combat dump layout: {e}");
                self.records = DUMP_MAX_RECORDS;
                return;
            }
            match std::fs::File::create(dir.join("combat_dump.bin")) {
                Ok(f) => self.file = Some(f),
                Err(e) => {
                    crate::error!("probe", "combat dump: {e}");
                    self.records = DUMP_MAX_RECORDS;
                    return;
                }
            }
            crate::info!("probe", "combat dump started every {DUMP_EVERY} frames: {}", text.join(", "));
        }
        let s = game::snapshot(player);
        let mut buf = Vec::with_capacity(16 + self.layout.iter().map(|l| l.1).sum::<usize>());
        buf.extend_from_slice(&frame.to_le_bytes());
        buf.extend_from_slice(&s.stamina.to_le_bytes());
        buf.extend_from_slice(&s.anim_id.to_le_bytes());
        for (_, ptr, len) in regions {
            // SAFETY: each region is a live object of exactly `len` bytes (dump_regions); main thread.
            buf.extend_from_slice(unsafe { std::slice::from_raw_parts(ptr, len) });
        }
        if let Some(f) = &mut self.file {
            let _ = f.write_all(&buf);
        }
        self.records += 1;
        if self.records == DUMP_MAX_RECORDS {
            crate::info!("probe", "combat dump full ({DUMP_MAX_RECORDS} records)");
        }
    }
}

impl SpEffectWatch {
    pub fn new() -> Self {
        Self { last: None, lines: 0 }
    }

    pub fn run(&mut self) {
        if self.lines >= MAX_LINES {
            return;
        }
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.last = None;
            return;
        };
        let effects = &player.chr_ins.special_effect;
        let now: BTreeSet<i32> = effects.entries().map(|e| e.param_id).collect();
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        let s = game::snapshot(player);
        match &self.last {
            None => {
                let all: Vec<String> = effects.entries().map(describe).collect();
                crate::info!("speffect", "frame={frame} spawn list ({}): {} | stamina={} anim={}", all.len(), all.join(", "), s.stamina, s.anim_id);
                self.lines += 1;
            }
            Some(prev) if *prev != now => {
                let added: Vec<String> = effects.entries().filter(|e| !prev.contains(&e.param_id)).map(describe).collect();
                let removed: Vec<String> = prev.difference(&now).map(i32::to_string).collect();
                crate::info!(
                    "speffect",
                    "frame={frame} +[{}] -[{}] | stamina={} anim={}",
                    added.join(", "),
                    removed.join(", "),
                    s.stamina,
                    s.anim_id
                );
                self.lines += 1;
                if self.lines == MAX_LINES {
                    crate::info!("speffect", "line limit reached; SpEffect watch stops");
                }
            }
            Some(_) => {}
        }
        self.last = Some(now);
    }
}
