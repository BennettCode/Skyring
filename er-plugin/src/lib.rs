//! skyrimxer_er: the Elden Ring half of Skyrim X Elden Ring. Loaded by me3 (see me3/skyrim-x-er.me3).
//!
//! Phase 1: the DLL loads, eldenring-rs accepts this game version, and a per-frame task runs.
//! Phase 2: a link thread keeps the shared-memory heartbeat/handshake with Skyrim going (skyrimxer-protocol).
//! Phase 3: hidden-but-focused window (window.rs), player state readout (game.rs). Plan: docs/P3-PLAN.md.

mod actions;
mod attack;
mod body;
mod bridge;
mod combat;
mod config;
mod focus;
mod game;
mod log;
mod pad;
mod park;
mod pose;
mod pose_stream;
mod remote;
mod stance;
mod window;

use std::ffi::c_void;
use std::panic;
use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use eldenring::cs::{CSTaskGroupIndex, CSTaskImp};
use eldenring::fd4::FD4TaskData;
use fromsoftware_shared::SharedTaskImpExt;
use windows::Win32::Foundation::HMODULE;
use windows::Win32::System::LibraryLoader::GetModuleFileNameW;

const DLL_PROCESS_DETACH: u32 = 0;
const DLL_PROCESS_ATTACH: u32 = 1;

/// # Safety
/// Called by the Windows loader only.
#[unsafe(no_mangle)]
pub unsafe extern "system" fn DllMain(hmodule: HMODULE, reason: u32, _reserved: *mut c_void) -> bool {
    match reason {
        DLL_PROCESS_ATTACH => {
            // Never do real work under the loader lock; hand off to a thread.
            let module = hmodule.0 as usize;
            std::thread::spawn(move || init(module));
        }
        // Clean game exit: tell Skyrim with a Bye (best effort; a crash is covered by the heartbeat timeout).
        DLL_PROCESS_DETACH => bridge::on_process_exit(),
        _ => {}
    }
    true
}

fn init(module: usize) {
    let dll_dir = module_dir(module);
    if let Some(dir) = &dll_dir {
        let _ = log::init(&dir.join("logs"));
    }
    panic::set_hook(Box::new(|info| error!("panic", "{info}")));

    info!(
        "core",
        "skyrimxer_er v{} loaded into {}",
        env!("CARGO_PKG_VERSION"),
        std::env::current_exe().map(|p| p.display().to_string()).unwrap_or_default()
    );
    if let Some(dir) = &dll_dir {
        config::load(dir);
    }

    // eldenring-rs resolves its RVA table here and panics if eldenring.exe isn't a supported version.
    let result = panic::catch_unwind(|| CSTaskImp::wait_for_instance(Duration::MAX));
    let task = match result {
        Ok(Ok(task)) => task,
        Ok(Err(e)) => return error!("core", "CSTaskImp unavailable: {e:?}"),
        Err(_) => return error!("core", "eldenring-rs rejected this game version; mod disabled"),
    };
    info!("core", "game version supported, task system ready");

    // Only after the version check: an unsupported ER never shows up as a peer, so Skyrim stays vanilla.
    bridge::start();

    let mut frame = FrameTask::new();
    let handle = task.run_recurring(move |_: &FD4TaskData| frame.run(), CSTaskGroupIndex::FrameBegin);
    // Dropping the handle cancels the task; it must live as long as the game.
    std::mem::forget(handle);

    // PadStep re-sets the background flag every frame while ER isn't foreground; clear it again before the characters read input.
    std::mem::forget(task.run_recurring(|_: &FD4TaskData| window::spoof_focus(), CSTaskGroupIndex::WorldChrMan_Prepare));

    let (group, group_name) = actions::inject_group();
    if actions::locotest_enabled() {
        let mut test = actions::LocoTest::new();
        std::mem::forget(task.run_recurring(move |_: &FD4TaskData| test.run(), group));
        info!("loco-test", "SELFTEST {}: locomotion script in {group_name} (Skyrim input ignored)", config::get().selftest);
    } else if actions::selftest_enabled() {
        let mut injector = actions::Injector::new();
        std::mem::forget(task.run_recurring(move |_: &FD4TaskData| injector.run(), group));
        info!("action", "SELFTEST dodge: injector in {group_name} (Skyrim input ignored)");
    } else {
        let mut dodge = remote::DodgeFromSkyrim::new();
        std::mem::forget(task.run_recurring(move |_: &FD4TaskData| dodge.run(), group));
        info!("core", "Skyrim input → dodge injector in {group_name}");
    }
    let mut watcher = actions::Watcher::new();
    std::mem::forget(task.run_recurring(move |_: &FD4TaskData| watcher.run(), CSTaskGroupIndex::ChrIns_PostPhysics));
    std::mem::forget(task.run_recurring(|_: &FD4TaskData| remote::sample_coords(), CSTaskGroupIndex::ChrIns_PostPhysics));
    if config::get().dump {
        let mut speffects = combat::SpEffectWatch::new();
        std::mem::forget(task.run_recurring(move |_: &FD4TaskData| speffects.run(), CSTaskGroupIndex::ChrIns_PostPhysics));
        info!("core", "SpEffect watch in ChrIns_PostPhysics (research, dump=1)");
    }
    // One task so PlayerState always carries this frame's virtual position (park.rs pins the character, publish_state sends it).
    let mut park = park::Park::new();
    std::mem::forget(task.run_recurring(
        move |_: &FD4TaskData| {
            park.run();
            remote::publish_state(false);
        },
        CSTaskGroupIndex::ChrIns_PostPhysics,
    ));
    let mut stance = stance::StanceSync::new();
    std::mem::forget(task.run_recurring(move |_: &FD4TaskData| stance.run(), CSTaskGroupIndex::ChrIns_PostPhysics));
    let mut pose_stream = pose_stream::PoseStream::new();
    std::mem::forget(task.run_recurring(move |_: &FD4TaskData| pose_stream.run(), CSTaskGroupIndex::ChrIns_PostPhysics));
    info!("core", "PoseState publisher in ChrIns_PostPhysics");
    let mut combat_watch = combat::CombatWatch::new();
    std::mem::forget(task.run_recurring(move |_: &FD4TaskData| combat_watch.run(), CSTaskGroupIndex::ChrIns_PostPhysics));
    let force = config::get().force_combat.as_str();
    if force == "on" || force == "off" {
        let on = force == "on";
        // ER recomputes the flag every frame in ChrIns_NaviCache (P4 step 2: written in every group WorldChrMan_Prepare..PostPhysics,
        // only NaviCache found it changed back). Written once per frame in ChrIns_AILogic, it holds through behavior, where dodges are charged.
        let mut f = combat::ForceCombat::new("ChrIns_AILogic", on);
        std::mem::forget(task.run_recurring(move |_: &FD4TaskData| f.run(), CSTaskGroupIndex::ChrIns_AILogic));
        info!("combat", "FORCE combat state {force} (ChrIns_AILogic, every frame)");
    } else {
        let mut mirror = combat::MirrorCombat::new();
        std::mem::forget(task.run_recurring(move |_: &FD4TaskData| mirror.run(), CSTaskGroupIndex::ChrIns_AILogic));
        info!("core", "Skyrim combat state → ER combat flag in ChrIns_AILogic");
    }
    if config::get().dump {
        let mut dump = combat::Dump::new();
        std::mem::forget(task.run_recurring(move |_: &FD4TaskData| dump.run(), CSTaskGroupIndex::ChrIns_PostPhysics));
    }
    if config::get().pose_probe {
        let mut probe = pose::PoseProbe::new();
        std::mem::forget(task.run_recurring(move |_: &FD4TaskData| probe.run(), CSTaskGroupIndex::ChrIns_PostPhysics));
        info!("pose", "pose probe in ChrIns_PostPhysics (research, pose_probe=1)");
    }
    info!("core", "PlayerState publisher in ChrIns_PostPhysics (FrameBegin while not in world), dodge watcher in ChrIns_PostPhysics");
    if config::get().probe {
        let last = actions::probe::GROUPS.len() - 1;
        for (i, (group, name)) in actions::probe::GROUPS.into_iter().enumerate() {
            std::mem::forget(task.run_recurring(move |_: &FD4TaskData| actions::probe::sample(name, i == last), group));
        }
        info!("probe", "sp_move probe on {} task groups", last + 1);
        std::mem::forget(task.run_recurring(|_: &FD4TaskData| focus::sample(1, "WPrep"), CSTaskGroupIndex::WorldChrMan_Prepare));
        info!("probe", "focus probe on FrameBegin + WorldChrMan_Prepare");
    }
    info!("core", "per-frame task registered (FrameBegin)");
}

/// FrameBegin: frame counter, focus spoof / window hiding, and a player-state line every STATE_LOG_INTERVAL.
struct FrameTask {
    in_world: bool,
    window: window::WindowControl,
    last_log: Instant,
    frames_at_last_log: u64,
}

const STATE_LOG_INTERVAL: Duration = Duration::from_secs(5);

impl FrameTask {
    fn new() -> Self {
        Self { in_world: false, window: window::WindowControl::new(), last_log: Instant::now(), frames_at_last_log: 0 }
    }

    fn run(&mut self) {
        let frames = bridge::FRAMES.fetch_add(1, Ordering::Relaxed) + 1;
        // SAFETY: task callbacks run on the game's main thread.
        let player = unsafe { game::main_player() };
        let in_world = player.is_some();
        if in_world != self.in_world {
            self.in_world = in_world;
            match &player {
                Some(p) => info!("core", "main player spawned (in world at {})", p.current_block_id),
                None => info!("core", "main player gone (menu/loading)"),
            }
        }
        if config::get().probe {
            focus::sample(0, "FrameBegin");
        }
        self.window.frame(in_world);
        remote::publish_state(true);

        let elapsed = self.last_log.elapsed();
        if elapsed >= STATE_LOG_INTERVAL {
            let fps = (frames - self.frames_at_last_log) as f64 / elapsed.as_secs_f64();
            let state = player.map_or_else(|| "no player".to_string(), |p| game::snapshot(p).line());
            info!("state", "frame={frames} fps={fps:.1} {state}");
            self.last_log = Instant::now();
            self.frames_at_last_log = frames;
        }
    }
}

/// Directory containing this DLL, so logs land next to the build output regardless of the game's working directory.
fn module_dir(module: usize) -> Option<PathBuf> {
    let mut buf = [0u16; 1024];
    let len = unsafe { GetModuleFileNameW(Some(HMODULE(module as *mut c_void)), &mut buf) } as usize;
    if len == 0 || len >= buf.len() {
        return None;
    }
    PathBuf::from(String::from_utf16_lossy(&buf[..len])).parent().map(PathBuf::from)
}
