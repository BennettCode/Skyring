//! Dev settings from `skyrimxer_er.cfg` next to the DLL (`key=value` lines, `#` comments). `tools/dev.ps1` rewrites it on
//! every launch (`-ErVisible`, `-ErSelfTest`, `-ErInjectGroup`), so a setting never lingers from an earlier run. Missing file = defaults.

use std::path::Path;
use std::sync::OnceLock;

#[derive(Debug, Default)]
pub struct Config {
    /// Keep ER's window visible when in the world (debugging).
    pub visible: bool,
    /// `dodge` = press Backstep every few seconds with no Skyrim involved (P3 step 2); `roll` = the same plus a move direction (step 5);
    /// `walk` / `sprint` = the locomotion scripts (actions::LocoTest, LOCO-PLAN stage B probe).
    pub selftest: String,
    /// Task group the action injection runs in: wprep (WorldChrMan_Prepare, default), padstep, ailogic, prebehavior.
    pub inject_group: String,
    /// Log sp_move bits at several task groups each frame (actions::probe).
    pub probe: bool,
    /// Research: dump player memory snapshots to logs/combat_dump.bin (combat::Dump) and log SpEffect changes (combat::SpEffectWatch).
    pub dump: bool,
    /// `on` / `off`: force ER's combat state every frame (combat::ForceCombat, P4 step 2 research). Empty = leave it to the game.
    pub force_combat: String,
    /// Pin the character to its spot (park.rs, P4 step 4b). `pin=0` lets it move freely (debugging with `visible=1`, A/B tests).
    pub pin: bool,
    /// Research: find the skeleton pose in memory (pose::PoseProbe, docs/POSE-PLAN.md step 1).
    pub pose_probe: bool,
    /// Research: write this weapon stance into ChrAsm once after spawn (stance.rs): empty | one | right2 | left2. Empty = don't write.
    pub stance: String,
}

static CONFIG: OnceLock<Config> = OnceLock::new();

pub fn load(dir: &Path) {
    let path = dir.join("skyrimxer_er.cfg");
    let mut config = Config { pin: true, ..Config::default() };
    if let Ok(text) = std::fs::read_to_string(&path) {
        for line in text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#')) {
            let Some((key, value)) = line.split_once('=') else { continue };
            let value = value.trim();
            match key.trim() {
                "visible" => config.visible = value == "1",
                "selftest" => config.selftest = value.to_string(),
                "inject_group" => config.inject_group = value.to_string(),
                "probe" => config.probe = value == "1",
                "dump" => config.dump = value == "1",
                "force_combat" => config.force_combat = value.to_string(),
                "pin" => config.pin = value != "0",
                "pose_probe" => config.pose_probe = value == "1",
                "stance" => config.stance = value.to_string(),
                other => crate::error!("core", "skyrimxer_er.cfg: unknown key `{other}`"),
            }
        }
    }
    crate::info!("core", "config {}: {config:?}", path.display());
    let _ = CONFIG.set(config);
}

pub fn get() -> &'static Config {
    CONFIG.get_or_init(|| Config { pin: true, ..Config::default() })
}
