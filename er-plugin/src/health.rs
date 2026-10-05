//! P5 step 3 (protocol v11): Skyrim's hits on the player land on ER's HP; ER's HP is the one that decides death.
//! Skyrim (`skse/src/bridge/Health.cpp`) refunds every health drop of its player and adds it to `InputState.hurt_total` as a share of
//! Skyrim's max health (healing as a negative share). Here the change of that running total since the last read becomes the same share
//! of ER's max HP. ER's HP never goes below 1: a hit that would take it to 0 sets PlayerFlag Downed instead, and Skyrim kills its
//! player (user choice 2026-10-05: no ER death screen or rune loss; the ER character refills when Skyrim loads a save, `respawn_seq`).
//! Main thread (ChrIns_PostPhysics task). Bridge off / stale input: nothing applied.
use std::sync::atomic::{AtomicBool, Ordering};

use skyrimxer_protocol::now_ms;
use skyrimxer_protocol::proto::{InputFlag, InputState, OFF_SLOT_INPUT};
use skyrimxer_protocol::slot::{SlotReader, fresh};

use crate::{bridge, game};

const BRIDGE_ON: u32 = 1 << InputFlag::BridgeOn as u32;

/// Read by `remote::publish_state` for PlayerFlag Downed.
pub static DOWNED: AtomicBool = AtomicBool::new(false);

/// ER HP after a change of `share` (of max HP; + damage, − healing): (new hp, downed by this change). Pure (unit tested).
pub fn apply(hp: i32, max_hp: i32, share: f64) -> (i32, bool) {
    let change = (share * max_hp as f64).round() as i64;
    let next = hp as i64 - change;
    if change > 0 && next < 1 {
        return (1, true);
    }
    (next.clamp(1, max_hp.max(1) as i64) as i32, false)
}

pub struct HealthSync {
    reader: Option<SlotReader<InputState>>,
    last_input: Option<InputState>,
    /// hurt_total already applied (None = prime on the next fresh read: a reconnect isn't a hit).
    applied: Option<f64>,
    respawn: Option<u32>,
    /// Share not yet applied because it was under one HP (Skyrim's regen and healing spells come in small steps every frame).
    carry: f64,
}

impl HealthSync {
    pub fn new() -> Self {
        Self { reader: None, last_input: None, applied: None, respawn: None, carry: 0.0 }
    }

    pub fn run(&mut self) {
        let Some(shared) = bridge::shared() else { return };
        if self.reader.is_none() {
            self.reader = shared.region().map(|r| SlotReader::new(r, OFF_SLOT_INPUT));
        }
        let Some(reader) = &self.reader else { return };
        self.last_input = reader.read().or(self.last_input);
        if !shared.connected() {
            // A new Skyrim session starts its running total at 0 again: prime from scratch (a pause or menu only makes the input stale and
            // keeps the total, so damage taken while Skyrim was paused, e.g. in the console, still arrives).
            self.applied = None;
            self.respawn = None;
            self.carry = 0.0;
            return;
        }
        let input = self.last_input.filter(|i| fresh(i.time_ms, now_ms()) && i.flags & BRIDGE_ON != 0);
        // SAFETY: main thread (task callback).
        let (Some(i), Some(player)) = (input, unsafe { game::main_player() }) else {
            return;
        };
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        let data = &mut *player.chr_ins.modules.data;
        if self.respawn != Some(i.respawn_seq) {
            if self.respawn.is_some() {
                data.hp = data.max_hp;
                DOWNED.store(false, Ordering::Relaxed);
                crate::info!("health", "frame={frame} respawn #{} (Skyrim loaded a save): HP refilled to {}", i.respawn_seq, data.max_hp);
            }
            self.respawn = Some(i.respawn_seq);
            self.applied = Some(i.hurt_total);
            return;
        }
        let Some(prev) = self.applied else {
            self.applied = Some(i.hurt_total);
            crate::info!("health", "frame={frame} Skyrim hits now land on ER's HP ({}/{}), hurt_total primed at {:.4}", data.hp, data.max_hp, i.hurt_total);
            return;
        };
        let share = i.hurt_total - prev + self.carry;
        self.applied = Some(i.hurt_total);
        if (share * data.max_hp as f64).abs() < 1.0 {
            self.carry = share;  // under one HP: keep it for the next frame
            return;
        }
        let whole = (share * data.max_hp as f64).trunc() / data.max_hp as f64;
        self.carry = share - whole;
        let share = whole;
        let before = data.hp;
        let (hp, downed) = apply(data.hp, data.max_hp, share);
        data.hp = hp;
        if downed {
            DOWNED.store(true, Ordering::Relaxed);
        }
        crate::info!(
            "health",
            "frame={frame} {} {:.1}% of max → ER HP {before} → {hp}/{}{}",
            if share > 0.0 { "hit" } else { "healed" },
            share.abs() * 100.0,
            data.max_hp,
            if downed { " — DOWNED (Skyrim kills the player)" } else { "" }
        );
    }
}

#[cfg(test)]
mod tests {
    use super::apply;

    #[test]
    fn hits_heals_and_downed() {
        assert_eq!(apply(1000, 1000, 0.25), (750, false), "a quarter of max");
        assert_eq!(apply(750, 1000, -0.5), (1000, false), "healing caps at max");
        assert_eq!(apply(100, 1000, 0.2), (1, true), "a hit past 0 holds at 1 and downs");
        assert_eq!(apply(1, 1000, -0.0001), (1, false), "a tiny heal rounds to nothing");
    }
}
