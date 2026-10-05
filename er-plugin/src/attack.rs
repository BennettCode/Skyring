//! P5 step 1, ER side: the swing's hit window and ER's numbers for it (PlayerState v9: AttackActive, attack_seq, atk_*).
//!
//! **Hit window** (stage A probe, `docs/research/elden-ring-combat.md`): `CSChrActionFlagModule.action_modifiers_flags` bit 9
//! (`invokeknockbackvalue`) or bit 10 (`parryable`), or the byte at module +0x1d0 != 0. Set for 2 frames (light) / 3 frames (heavy
//! release) at 60 fps, in the fastest part of the sweep, in every swing measured. ER exposes no AtkParam row id for the swing (none found
//! in the dumped modules), so the motion value comes from the attack kind (anim group) instead of the swing's own AtkParam row.
//!
//! **Attack rating** = ER's weapon formula from its params: per damage type, base (EquipParamWeapon attack_base_*) x upgrade rate
//! (ReinforceParamWeapon *_atk_rate, row = reinforce_type_id + upgrade level), plus a scaling bonus per stat that the
//! AttackElementCorrectParam row lets scale it: base x correct_<stat> x correct_<stat>_rate / 100 x CalcCorrectGraph(stat) / 100.
//! Two-handing counts strength x1.5. Computed when the weapon, its upgrade or the stats change, and logged with every input.
use std::sync::Mutex;

use eldenring::cs::{
    AttackElementCorrectParam, CalcCorrectGraph, ChrAsmHand, EquipParamWeapon, PlayerIns, ReinforceParamWeapon, SoloParamRepository,
};
use fromsoftware_shared::FromStatic;
use skyrimxer_protocol::proto::AttackKind;

/// Motion values (% of attack rating) and poise multipliers per attack kind: ER's usual values for a first light / uncharged heavy
/// swing. Tunable later (config) once the user has fought with them.
fn motion(kind: AttackKind) -> (f32, f32) {
    match kind {
        AttackKind::Light => (100.0, 1.0),
        AttackKind::Heavy => (140.0, 1.6),
        AttackKind::Skill => (150.0, 1.5),
        AttackKind::Other => (110.0, 1.2),
        _ => (0.0, 0.0),
    }
}

pub fn kind_of(anim: i32) -> AttackKind {
    if anim < 0 {
        return AttackKind::None;
    }
    let a = anim % 1_000_000;
    // One-handed 300xx / 305xx, two-handed 320xx / 325xx (live test 2026-10-05: greatsword two-handed R1 = 24032000, R2 = 120032505).
    match a / 100 {
        300 | 320 => AttackKind::Light,
        305 | 325 => AttackKind::Heavy,
        _ if a / 10_000 == 4 => AttackKind::Skill,
        _ if a / 10_000 == 3 => AttackKind::Other,
        _ => AttackKind::None,
    }
}

/// Attack rating per type: physical, magic, fire, lightning, holy; plus poise (weapon sa damage x upgrade rate).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Rating {
    pub ar: [f32; 5],
    pub poise: f32,
}

/// The swing as PlayerState carries it.
#[derive(Clone, Copy, Debug)]
pub struct Swing {
    pub active: bool,
    pub seq: u32,
    pub atk: [f32; 5],
    pub poise: f32,
    pub kind: AttackKind,
}

struct State {
    seq: u32,
    was_active: bool,
    /// Anim id and play time of the last window: a second window in the same play of the same anim (two-handed swings open two,
    /// ~0.25 s apart) is the same swing, so a target isn't hit twice by one swing.
    window_anim: i32,
    window_time: f32,
    key: Option<(i32, [u32; 5], bool)>,
    rating: Rating,
    last: Swing,
}

static STATE: Mutex<State> = Mutex::new(State { seq: 0, was_active: false, window_anim: -1, window_time: 0.0, key: None, rating: Rating { ar: [0.0; 5], poise: 0.0 }, last: Swing {
    active: false,
    seq: 0,
    atk: [0.0; 5],
    poise: 0.0,
    kind: AttackKind::None,
} });

/// ER's stat-scaling curve (CalcCorrectGraph): stage thresholds, growth % at each, exponent per stage. Returns growth in %.
fn curve(g: &eldenring::param::CACL_CORRECT_GRAPH_ST, stat: f32) -> f32 {
    let val = [g.stage_max_val0(), g.stage_max_val1(), g.stage_max_val2(), g.stage_max_val3(), g.stage_max_val4()];
    let grow = [g.stage_max_grow_val0(), g.stage_max_grow_val1(), g.stage_max_grow_val2(), g.stage_max_grow_val3(), g.stage_max_grow_val4()];
    let adj = [g.adj_pt_max_grow_val0(), g.adj_pt_max_grow_val1(), g.adj_pt_max_grow_val2(), g.adj_pt_max_grow_val3()];
    if stat <= val[0] {
        return grow[0];
    }
    for i in 0..4 {
        if stat <= val[i + 1] {
            let span = (val[i + 1] - val[i]).max(1e-6);
            let r = (stat - val[i]) / span;
            let r = if adj[i] > 0.0 { r.powf(adj[i]) } else { 1.0 - (1.0 - r).powf(-adj[i]) };
            return grow[i] + (grow[i + 1] - grow[i]) * r;
        }
    }
    grow[4]
}

/// Right-hand weapon id (with upgrade level), the five scaling stats (str, dex, int, fai, arc) and two-handing.
fn equipment(player: &PlayerIns) -> Option<(i32, [u32; 5], bool)> {
    // SAFETY: the main player's PlayerGameData lives as long as the player; main thread.
    let data = unsafe { player.player_game_data.as_ref() };
    let asm = &player.chr_asm;
    let slot = asm.equipment.active_weapon_slot(ChrAsmHand::Right) as usize;
    let weapon = *asm.equipment_param_ids.get(slot)?;
    Some((weapon, [data.strength, data.dexterity, data.intelligence, data.faith, data.arcane], asm.equipment.is_two_handing()))
}

fn compute(weapon: i32, stats: [u32; 5], two_handed: bool) -> Option<Rating> {
    // SAFETY: main thread; the param repository lives for the whole game.
    let repo = unsafe { SoloParamRepository::instance() }.ok()?;
    let base_id = (weapon / 100 * 100) as u32;
    let level = (weapon % 100) as u32;
    let w = repo.get::<EquipParamWeapon>(base_id)?;
    let r = repo.get::<ReinforceParamWeapon>(w.reinforce_type_id() as u32 + level)?;
    let e = repo.get::<AttackElementCorrectParam>(w.attack_element_correct_id() as u32);
    let base = [
        w.attack_base_physics() as f32 * r.physics_atk_rate(),
        w.attack_base_magic() as f32 * r.magic_atk_rate(),
        w.attack_base_fire() as f32 * r.fire_atk_rate(),
        w.attack_base_thunder() as f32 * r.thunder_atk_rate(),
        w.attack_base_dark() as f32 * r.dark_atk_rate(),
    ];
    let correct = [
        w.correct_strength() * r.correct_strength_rate(),
        w.correct_agility() * r.correct_agility_rate(),
        w.correct_magic() * r.correct_magic_rate(),
        w.correct_faith() * r.correct_faith_rate(),
        w.correct_luck() * r.correct_luck_rate(),
    ];
    let graphs = [w.correct_type_physics(), w.correct_type_magic(), w.correct_type_fire(), w.correct_type_thunder(), w.correct_type_dark()];
    let mut s = stats.map(|v| v as f32);
    if two_handed {
        s[0] = (s[0] * 1.5).min(148.0);
    }
    let mut ar = base;
    for d in 0..5 {
        if base[d] <= 0.0 {
            continue;
        }
        let Some(graph) = repo.get::<CalcCorrectGraph>(graphs[d] as u32) else { continue };
        for (k, stat) in s.iter().enumerate() {
            let scales = e.is_some_and(|e| match (k, d) {
                (0, 0) => e.is_strength_correct_by_physics(),
                (0, 1) => e.is_strength_correct_by_magic(),
                (0, 2) => e.is_strength_correct_by_fire(),
                (0, 3) => e.is_strength_correct_by_thunder(),
                (0, 4) => e.is_strength_correct_by_dark(),
                (1, 0) => e.is_dexterity_correct_by_physics(),
                (1, 1) => e.is_dexterity_correct_by_magic(),
                (1, 2) => e.is_dexterity_correct_by_fire(),
                (1, 3) => e.is_dexterity_correct_by_thunder(),
                (1, 4) => e.is_dexterity_correct_by_dark(),
                (2, 0) => e.is_magic_correct_by_physics(),
                (2, 1) => e.is_magic_correct_by_magic(),
                (2, 2) => e.is_magic_correct_by_fire(),
                (2, 3) => e.is_magic_correct_by_thunder(),
                (2, 4) => e.is_magic_correct_by_dark(),
                (3, 0) => e.is_faith_correct_by_physics(),
                (3, 1) => e.is_faith_correct_by_magic(),
                (3, 2) => e.is_faith_correct_by_fire(),
                (3, 3) => e.is_faith_correct_by_thunder(),
                (3, 4) => e.is_faith_correct_by_dark(),
                (4, 0) => e.is_luck_correct_by_physics(),
                (4, 1) => e.is_luck_correct_by_magic(),
                (4, 2) => e.is_luck_correct_by_fire(),
                (4, 3) => e.is_luck_correct_by_thunder(),
                (4, 4) => e.is_luck_correct_by_dark(),
                _ => false,
            });
            if scales && correct[k] > 0.0 {
                ar[d] += base[d] * correct[k] / 100.0 * curve(graph, *stat) / 100.0;
            }
        }
    }
    let rating = Rating { ar, poise: w.sa_weapon_damage() * r.sa_weapon_atk_rate() };
    crate::info!(
        "attack",
        "weapon {weapon} (+{level}, {}) stats str/dex/int/fai/arc={stats:?}: base={:.0?} correct={:.0?} graphs={graphs:?} → AR phys {:.0} mag {:.0} fire {:.0} lightning {:.0} holy {:.0}, poise {:.1}",
        if two_handed { "two-handed" } else { "one-handed" },
        base,
        correct,
        rating.ar[0],
        rating.ar[1],
        rating.ar[2],
        rating.ar[3],
        rating.ar[4],
        rating.poise
    );
    Some(rating)
}

/// Every frame from `remote::publish_state` (ChrIns_PostPhysics, main thread): the hit window and the swing's numbers.
pub fn sample(player: &PlayerIns, anim: i32) -> Swing {
    let mut st = STATE.lock().unwrap_or_else(|p| p.into_inner());
    if let Some(key) = equipment(player)
        && st.key != Some(key)
    {
        st.key = Some(key);
        st.rating = compute(key.0, key.1, key.2).unwrap_or_default();
    }
    let flags = &*player.chr_ins.modules.action_flag;
    // SAFETY: CSChrActionFlagModule is 600 bytes (eldenring-rs layout); main thread.
    let (mods, b1d0) = unsafe {
        let base = flags as *const _ as *const u8;
        ((base.add(0x40) as *const u64).read_unaligned(), *base.add(0x1d0))
    };
    let kind = kind_of(anim);
    let active = kind != AttackKind::None && ((mods >> 9) & 1 == 1 || (mods >> 10) & 1 == 1 || b1d0 != 0);
    let anims = &player.chr_ins.modules.time_act;
    let play_time = anims.anim_queue.get(anims.read_idx as usize).map_or(0.0, |a| a.play_time);
    let same_swing = anim == st.window_anim && play_time >= st.window_time;
    if active && !st.was_active {
        st.window_anim = anim;
        st.window_time = play_time;
    }
    if active && !st.was_active && same_swing {
        crate::info!("attack", "second window in the same swing (anim {anim}, t={play_time:.3}): same hit #{}", st.seq);
    }
    if !active && !same_swing {
        st.window_anim = -1;
    }
    if active && !st.was_active && !same_swing {
        st.seq = st.seq.wrapping_add(1);
        let (mv, poise_mult) = motion(kind);
        st.last = Swing {
            active: true,
            seq: st.seq,
            atk: st.rating.ar.map(|v| v * mv / 100.0),
            poise: st.rating.poise * poise_mult,
            kind,
        };
        crate::info!(
            "attack",
            "hit window #{} anim {anim} ({kind:?}, MV {mv}): phys {:.0} mag {:.0} fire {:.0} lightning {:.0} holy {:.0} poise {:.1}",
            st.seq,
            st.last.atk[0],
            st.last.atk[1],
            st.last.atk[2],
            st.last.atk[3],
            st.last.atk[4],
            st.last.poise
        );
    }
    st.was_active = active;
    st.last.active = active;
    st.last
}
