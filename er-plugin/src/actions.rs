//! Action injection: ER performs actions because we hold its in-game keys on the virtual pad (pad.rs). ER's own pipeline and gating
//! (stamina, recovery, tap = roll on release / hold = dash) still decide what happens. Writing `action_requests` directly does NOT
//! work: the engine rebuilds it from the pad every ChrIns_PreBehavior (probe result, docs/research/elden-ring-input.md).
//!
//! P3 step 2 self-test (`selftest=dodge` in skyrimxer_er.cfg): hold Backstep for PULSE_FRAMES frames every PULSE_EVERY frames, then
//! release it, and watch the player for WATCH_FRAMES frames from a ChrIns_PostPhysics task, logging every change.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use eldenring::cs::{CSTaskGroupIndex, UserInputKey};

use crate::{game, pad};

const PULSE_FRAMES: u32 = 4;
const PULSE_EVERY: u64 = 240;
const WATCH_FRAMES: u32 = 120;

/// Frames left in the current press (set by the injector, read by the watcher for logging).
static PULSE_LEFT: AtomicU32 = AtomicU32::new(0);
static WATCH_LEFT: AtomicU32 = AtomicU32::new(0);
static PULSES: AtomicU32 = AtomicU32::new(0);
static INJECT_FRAME: AtomicU64 = AtomicU64::new(0);

/// The task group the injector runs in, from `inject_group` in the config.
pub fn inject_group() -> (CSTaskGroupIndex, &'static str) {
    match crate::config::get().inject_group.as_str() {
        "padstep" => (CSTaskGroupIndex::PadStep, "PadStep"),
        "ailogic" => (CSTaskGroupIndex::ChrIns_AILogic, "ChrIns_AILogic"),
        "prebehavior" => (CSTaskGroupIndex::ChrIns_PreBehavior, "ChrIns_PreBehavior"),
        _ => (CSTaskGroupIndex::WorldChrMan_Prepare, "WorldChrMan_Prepare"),
    }
}

pub fn selftest_enabled() -> bool {
    crate::config::get().selftest == "dodge"
}

/// Runs in `inject_group()`. Self-test only for now; P3 step 4 feeds it from Skyrim's InputState.
pub struct Injector {
    frames: u64,
}

impl Injector {
    pub fn new() -> Self {
        Self { frames: 0 }
    }

    pub fn run(&mut self) {
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.frames = 0;
            return;
        };
        self.frames += 1;
        let frame = crate::bridge::FRAMES.load(Ordering::Relaxed);
        if self.frames % PULSE_EVERY == 0 && PULSE_LEFT.load(Ordering::Relaxed) == 0 {
            let n = PULSES.fetch_add(1, Ordering::Relaxed) + 1;
            let s = game::snapshot(player);
            // SAFETY: main thread.
            let slots = unsafe { pad::digital_slots(UserInputKey::Backstep) };
            crate::info!(
                "action",
                "frame={frame} selftest pulse #{n}: hold Backstep {PULSE_FRAMES} frames then release; before: stamina={} anim={} \
                 slots (mapped, index, checked)={slots:?}",
                s.stamina,
                s.anim_id
            );
            PULSE_LEFT.store(PULSE_FRAMES, Ordering::Relaxed);
            WATCH_LEFT.store(WATCH_FRAMES, Ordering::Relaxed);
            INJECT_FRAME.store(frame, Ordering::Relaxed);
        }
        let left = PULSE_LEFT.load(Ordering::Relaxed);
        if left > 0 {
            // SAFETY: main thread. Held while left > 0; the frame it reaches 0 writes the release.
            unsafe { pad::set_digital(UserInputKey::Backstep, left > 1) };
            PULSE_LEFT.store(left - 1, Ordering::Relaxed);
        }
    }
}

/// Runs in ChrIns_PostPhysics (after behavior): logs what the injected press did.
pub struct Watcher {
    last: Option<game::Snapshot>,
}

impl Watcher {
    pub fn new() -> Self {
        Self { last: None }
    }

    pub fn run(&mut self) {
        let left = WATCH_LEFT.load(Ordering::Relaxed);
        if left == 0 {
            self.last = None;
            return;
        }
        WATCH_LEFT.store(left - 1, Ordering::Relaxed);
        // SAFETY: main thread.
        let Some(player) = (unsafe { game::main_player() }) else { return };
        let s = game::snapshot(player);
        let ar = &player.chr_ins.modules.action_request;
        let frame = crate::bridge::FRAMES.load(Ordering::Relaxed);
        let since = frame.saturating_sub(INJECT_FRAME.load(Ordering::Relaxed));
        let pressing = PULSE_LEFT.load(Ordering::Relaxed) > 0 || since < PULSE_FRAMES as u64;
        let changed = match &self.last {
            None => true,
            Some(l) => l.anim_id != s.anim_id || l.iframe != s.iframe || l.dodging != s.dodging || l.stamina != s.stamina,
        };
        if changed || pressing {
            crate::info!(
                "action",
                "frame={frame} +{since}: stamina={} anim={} iframe={} dodging={} | requests.sp_move={} new_presses.sp_move={} \
                 readback_new.sp_move={}",
                s.stamina,
                s.anim_id,
                s.iframe as u8,
                s.dodging as u8,
                ar.action_requests.sp_move() as u8,
                ar.new_action_presses.sp_move() as u8,
                ar.readback_new_presses.sp_move() as u8
            );
        }
        if left == 1 {
            crate::info!("action", "frame={frame} watch window over: {}", s.line());
        }
        self.last = Some(s);
    }
}

/// Probe (`probe=1`): records sp_move's request / new-press / possible bits at several points of the frame and logs one line
/// per frame in which any of them is set. Shows where the engine fills and consumes `action_requests` (P3 step 2 research).
pub mod probe {
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicU32, Ordering};

    use eldenring::cs::CSTaskGroupIndex;

    use crate::game;

    pub const GROUPS: [(CSTaskGroupIndex, &str); 9] = [
        (CSTaskGroupIndex::PadStep, "Pad"),
        (CSTaskGroupIndex::WorldChrMan_Prepare, "WPrep"),
        (CSTaskGroupIndex::ChrIns_CalcUpdateInfo, "Calc"),
        (CSTaskGroupIndex::ChrIns_AILogic, "AI"),
        (CSTaskGroupIndex::ChrIns_PreBehavior, "PreB"),
        (CSTaskGroupIndex::ChrIns_PreBehaviorSafe, "PreBS"),
        (CSTaskGroupIndex::HavokBehavior, "Havok"),
        (CSTaskGroupIndex::ChrIns_PrePhysics, "PreP"),
        (CSTaskGroupIndex::ChrIns_PostPhysics, "PostP"),
    ];
    const MAX_LINES: u32 = 400;

    static ROW: Mutex<Vec<String>> = Mutex::new(Vec::new());
    static ANY: Mutex<bool> = Mutex::new(false);
    static LINES: AtomicU32 = AtomicU32::new(0);

    pub fn sample(name: &'static str, last: bool) {
        // SAFETY: task callbacks run on the main thread.
        let Some(player) = (unsafe { game::main_player() }) else { return };
        let ar = &player.chr_ins.modules.action_request;
        let (r, n, p, pr) = (
            ar.action_requests.sp_move(),
            ar.new_action_presses.sp_move(),
            ar.possible_action_inputs.sp_move(),
            ar.previous_action_requests.sp_move(),
        );
        let mut row = ROW.lock().unwrap_or_else(|e| e.into_inner());
        let mut any = ANY.lock().unwrap_or_else(|e| e.into_inner());
        // K = what ER's in-game pad reports for the Backstep key (the virtual input we write).
        // SAFETY: main thread.
        let k = unsafe { crate::pad::poll(eldenring::cs::UserInputKey::Backstep) }.unwrap_or(false);
        row.push(format!("{name}:K{}R{}N{}P{}r{}", k as u8, r as u8, n as u8, p as u8, pr as u8));
        *any |= r || n || pr || k;
        if last {
            if *any && LINES.fetch_add(1, Ordering::Relaxed) < MAX_LINES {
                let frame = crate::bridge::FRAMES.load(Ordering::Relaxed);
                let s = game::snapshot(player);
                crate::info!(
                    "probe",
                    "frame={frame} {} | anim={} stamina={} dodging={}",
                    row.join(" "),
                    s.anim_id,
                    s.stamina,
                    s.dodging as u8
                );
            }
            row.clear();
            *any = false;
        }
    }
}
