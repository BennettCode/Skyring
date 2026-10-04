//! Dev settings from `skyrimxer_er.cfg` next to the DLL (`key=value` lines, `#` comments). `tools/dev.ps1` rewrites it on
//! every launch (`-ErVisible`, `-ErSelfTest`, `-ErInjectGroup`), so a setting never lingers from an earlier run. Missing file = defaults.

use std::path::Path;
use std::sync::OnceLock;

#[derive(Debug, Default)]
pub struct Config {
    /// Keep ER's window visible when in the world (debugging).
    pub visible: bool,
    /// `dodge` = press Backstep every few seconds with no Skyrim involved (P3 step 2); `roll` = the same plus a move direction (step 5).
    pub selftest: String,
    /// Task group the action injection runs in: wprep (WorldChrMan_Prepare, default), padstep, ailogic, prebehavior.
    pub inject_group: String,
    /// Log sp_move bits at several task groups each frame (actions::probe).
    pub probe: bool,
}

static CONFIG: OnceLock<Config> = OnceLock::new();

pub fn load(dir: &Path) {
    let path = dir.join("skyrimxer_er.cfg");
    let mut config = Config::default();
    if let Ok(text) = std::fs::read_to_string(&path) {
        for line in text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#')) {
            let Some((key, value)) = line.split_once('=') else { continue };
            let value = value.trim();
            match key.trim() {
                "visible" => config.visible = value == "1",
                "selftest" => config.selftest = value.to_string(),
                "inject_group" => config.inject_group = value.to_string(),
                "probe" => config.probe = value == "1",
                other => crate::error!("core", "skyrimxer_er.cfg: unknown key `{other}`"),
            }
        }
    }
    crate::info!("core", "config {}: {config:?}", path.display());
    let _ = CONFIG.set(config);
}

pub fn get() -> &'static Config {
    CONFIG.get_or_init(Config::default)
}
