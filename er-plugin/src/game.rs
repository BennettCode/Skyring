//! Read-only views of the ER main player (field notes: docs/research/elden-ring-state.md). Main thread only (task callbacks).

use eldenring::cs::{PlayerIns, WorldChrMan};
use fromsoftware_shared::FromStatic;

/// One frame's worth of the player state Skyrim cares about.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Snapshot {
    pub hp: i32,
    pub max_hp: i32,
    pub fp: i32,
    pub max_fp: i32,
    pub stamina: i32,
    pub max_stamina: i32,
    /// Animation currently playing (TAE queue read slot).
    pub anim_id: i32,
    /// TAE PERFECT_INVINCIBILITY: the i-frame window.
    pub iframe: bool,
    /// TAE FLAG_AS_DODGING.
    pub dodging: bool,
    /// TAE SUPER_ARMOR.
    pub hyperarmor: bool,
    pub poise: f32,
    pub poise_max: f32,
    pub poise_broken: bool,
    /// Havok world position (Y-up, metres).
    pub pos: [f32; 3],
    /// Yaw in radians (rotation around Y).
    pub yaw: f32,
    pub block_id: i32,
}

/// The main player while it exists in the world (`None` at the title screen / loading).
///
/// # Safety
/// Main thread only (a task callback): the game mutates this object every frame.
pub unsafe fn main_player() -> Option<&'static mut PlayerIns> {
    unsafe { WorldChrMan::instance_mut() }.ok()?.main_player.as_deref_mut()
}

pub fn snapshot(player: &PlayerIns) -> Snapshot {
    let m = &player.chr_ins.modules;
    let flags = m.action_flag.action_modifiers_flags;
    let anims = &m.time_act;
    let anim_id = anims.anim_queue.get(anims.read_idx as usize).map_or(-1, |a| a.anim_id);
    let p = m.physics.position;
    Snapshot {
        hp: m.data.hp,
        max_hp: m.data.max_hp,
        fp: m.data.fp,
        max_fp: m.data.max_fp,
        stamina: m.data.stamina,
        max_stamina: m.data.max_stamina,
        anim_id,
        iframe: flags.perfect_invincibility(),
        dodging: flags.dodging(),
        hyperarmor: flags.super_armor(),
        poise: m.super_armor.sa_durability,
        poise_max: m.super_armor.sa_durability_max,
        poise_broken: m.super_armor.poise_broken_state,
        pos: [p.0, p.1, p.2],
        yaw: m.physics.orientation.to_euler_angles().1,
        block_id: player.current_block_id.into(),
    }
}

impl Snapshot {
    /// Compact one-line form for the logs.
    pub fn line(&self) -> String {
        format!(
            "hp={}/{} fp={}/{} stamina={}/{} anim={} iframe={} dodging={} hyperarmor={} poise={:.1}/{:.1}{} \
             pos=({:.2},{:.2},{:.2}) yaw={:.3} map={}",
            self.hp,
            self.max_hp,
            self.fp,
            self.max_fp,
            self.stamina,
            self.max_stamina,
            self.anim_id,
            self.iframe as u8,
            self.dodging as u8,
            self.hyperarmor as u8,
            self.poise,
            self.poise_max,
            if self.poise_broken { " BROKEN" } else { "" },
            self.pos[0],
            self.pos[1],
            self.pos[2],
            self.yaw,
            eldenring::cs::BlockId::from(self.block_id)
        )
    }
}
