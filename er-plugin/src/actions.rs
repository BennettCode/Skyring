//! Action injection: ER performs actions because we hold its in-game keys on the virtual pad (pad.rs). ER's own pipeline and gating
//! (stamina, recovery, tap = roll on release / hold = dash) still decide what happens. Writing `action_requests` directly does NOT
//! work: the engine rebuilds it from the pad every ChrIns_PreBehavior (probe result, docs/research/elden-ring-input.md).
//!
//! P3 step 2 self-test (`selftest=dodge` in skyrimxer_er.cfg): every PULSE_EVERY frames hold Backstep for the next length in
//! HOLD_FRAMES, with BackstepTapped on the press frame only (what a real tap does), then release it, and watch the player for
//! WATCH_FRAMES frames from a ChrIns_PostPhysics task, logging every change.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use eldenring::cs::{CSTaskGroupIndex, UserInputKey};

use crate::{game, pad};

/// Hold lengths cycled per pulse (frames).
const HOLD_FRAMES: [u32; 3] = [4, 9, 30];
const PULSE_EVERY: u64 = 240;
const WATCH_FRAMES: u32 = 120;

/// Frames left in the current press (set by the injector, read by the watcher for logging).
static PULSE_LEFT: AtomicU32 = AtomicU32::new(0);
/// Hold length of the current press, to find its first frame.
static PULSE_HOLD: AtomicU32 = AtomicU32::new(0);
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
            let hold = HOLD_FRAMES[(n as usize - 1) % HOLD_FRAMES.len()];
            crate::info!(
                "action",
                "frame={frame} selftest pulse #{n}: hold Backstep {hold} frames then release; before: stamina={} anim={} \
                 slots (mapped, index, checked)={slots:?}",
                s.stamina,
                s.anim_id
            );
            PULSE_HOLD.store(hold, Ordering::Relaxed);
            PULSE_LEFT.store(hold + 1, Ordering::Relaxed);
            WATCH_LEFT.store(WATCH_FRAMES, Ordering::Relaxed);
            INJECT_FRAME.store(frame, Ordering::Relaxed);
        }
        let left = PULSE_LEFT.load(Ordering::Relaxed);
        if left > 0 {
            // SAFETY: main thread. Held while left > 0; the frame it reaches 0 writes the release.
            unsafe { pad::set_digital(UserInputKey::Backstep, left > 1) };
            // A real tap also raises BackstepTapped for exactly the press frame; without it the release is not turned into a
            // backstep/roll request (probe, docs/research/elden-ring-input.md session 3).
            let first = left == PULSE_HOLD.load(Ordering::Relaxed) + 1;
            // SAFETY: main thread.
            unsafe { pad::set_digital(UserInputKey::BackstepTapped, first) };
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
        let pressing = PULSE_LEFT.load(Ordering::Relaxed) > 0 || since < 3;
        let changed = match &self.last {
            None => true,
            Some(l) => l.anim_id != s.anim_id || l.iframe != s.iframe || l.dodging != s.dodging || l.stamina != s.stamina,
        };
        if changed || pressing {
            crate::info!(
                "action",
                "frame={frame} +{since}: stamina={} anim={} iframe={} dodging={} | requests.sp_move={} new_presses.sp_move={} \
                 readback_new.sp_move={} roll_timer={:.3}",
                s.stamina,
                s.anim_id,
                s.iframe as u8,
                s.dodging as u8,
                ar.action_requests.sp_move() as u8,
                ar.new_action_presses.sp_move() as u8,
                ar.readback_new_presses.sp_move() as u8,
                ar.action_timers.roll
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
    use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};

    use eldenring::cs::{CSTaskGroupIndex, UserInputKey};

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
    const MASK_EVERY: u64 = 120;
    const MAX_MASK_LINES: u32 = 200;
    static MASK_LINES: AtomicU32 = AtomicU32::new(0);
    const MAX_WIDE_LINES: u32 = 800;
    static WIDE_LINES: AtomicU32 = AtomicU32::new(0);
    static DESCRIBED: AtomicBool = AtomicBool::new(false);
    static LAST_ANIM: AtomicI32 = AtomicI32::new(-1);
    /// ChrActions bits watched by the wide line: sp_move, backstep, rolling, emergencystep.
    const DODGE_BITS: u64 = 1 << 5 | 1 << 16 | 1 << 17 | 1 << 25;

    fn bits(a: eldenring::cs::ChrActions) -> u64 {
        // SAFETY: ChrActions is a one-field bitfield over a u64 (its inner field is private).
        unsafe { std::mem::transmute(a) }
    }

    /// Whole-mask snapshot of the engine's input gating (end of frame). `queued = new_presses & possible` per eldenring-rs, so a
    /// press with `possible` clear is dropped: this line shows which actions the current state allows.
    fn log_masks(player: &eldenring::cs::PlayerIns, frame: u64) {
        let ar = &player.chr_ins.modules.action_request;
        let flags: u32 = {
            let f = player.chr_ins.modules.action_flag.animation_action_flags;
            // SAFETY: ChrActionAnimationFlags is a one-field bitfield over a u32.
            unsafe { std::mem::transmute(f) }
        };
        crate::info!(
            "probe",
            "frame={frame} masks: possible={:#x} prev_possible={:#x} cancels={:#x} disabled={:#x} queued={:#x} requests={:#x} \
             anim_flags={flags:#x} anim={}",
            bits(ar.possible_action_inputs),
            bits(ar.prev_possible_action_inputs),
            bits(ar.possible_action_cancels),
            bits(ar.disabled_action_inputs),
            bits(ar.queued_action_inputs),
            bits(ar.action_requests),
            game::snapshot(player).anim_id
        );
    }

    pub fn sample(name: &'static str, last: bool) {
        // SAFETY: task callbacks run on the main thread.
        let Some(player) = (unsafe { game::main_player() }) else { return };
        let ar = &player.chr_ins.modules.action_request;
        let (r, n, p, q, pr) = (
            ar.action_requests.sp_move(),
            ar.new_action_presses.sp_move(),
            ar.possible_action_inputs.sp_move(),
            ar.queued_action_inputs.sp_move(),
            ar.previous_action_requests.sp_move(),
        );
        let mut row = ROW.lock().unwrap_or_else(|e| e.into_inner());
        let mut any = ANY.lock().unwrap_or_else(|e| e.into_inner());
        // K = what ER's in-game pad reports for the Backstep key (the virtual input we write).
        // SAFETY: main thread.
        let k = unsafe { crate::pad::poll(UserInputKey::Backstep) }.unwrap_or(false);
        // T = the tap variant of the same key; B/L = the backstep / rolling request bits.
        // SAFETY: main thread.
        let t = unsafe { crate::pad::poll(UserInputKey::BackstepTapped) }.unwrap_or(false);
        let (b, l) = (ar.action_requests.backstep(), ar.action_requests.rolling());
        row.push(format!(
            "{name}:K{}T{}R{}N{}P{}Q{}r{}B{}L{}",
            k as u8, t as u8, r as u8, n as u8, p as u8, q as u8, pr as u8, b as u8, l as u8
        ));
        *any |= r || n || pr || k || t || b || l;
        if last {
            let frame = crate::bridge::FRAMES.load(Ordering::Relaxed);
            if !DESCRIBED.swap(true, Ordering::Relaxed) {
                describe_keys();
            }
            log_wide(player, frame);
            if frame % MASK_EVERY == 0 && MASK_LINES.fetch_add(1, Ordering::Relaxed) < MAX_MASK_LINES {
                log_masks(player, frame);
            }
            if *any && LINES.fetch_add(1, Ordering::Relaxed) < MAX_LINES {
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

    /// Logs every input-type group entry of the keys a dodge or roll might read (once, first frame in world).
    fn describe_keys() {
        use UserInputKey::*;
        for key in [Backstep, BackstepTapped, MovementControl, MoveForwards, MoveBackwards, MoveLeft, MoveRight] {
            // SAFETY: main thread.
            let entries = unsafe { crate::pad::describe(key) };
            crate::info!("probe", "keys {key:?}: (mapped, type, virtual index, checked)={entries:?}");
        }
    }

    /// One line per frame (end of frame) while anything dodge-related is set or the anim changes: full action masks, all
    /// key polls and the Backstep virtual input in each device layer. Compares a real tap with ours (P3 step 2, session 3).
    fn log_wide(player: &eldenring::cs::PlayerIns, frame: u64) {
        let ar = &player.chr_ins.modules.action_request;
        let anim = game::snapshot(player).anim_id;
        let anim_changed = LAST_ANIM.swap(anim, Ordering::Relaxed) != anim;
        // SAFETY: main thread.
        let polls = unsafe { crate::pad::poll_mask() };
        let tap_keys = 1 << UserInputKey::Backstep as i32 | 1 << UserInputKey::BackstepTapped as i32;
        let masks = bits(ar.action_requests) | bits(ar.new_action_presses) | bits(ar.released_actions) | bits(ar.previous_action_requests);
        if !(anim_changed || polls & tap_keys != 0 || masks & DODGE_BITS != 0) {
            return;
        }
        if WIDE_LINES.fetch_add(1, Ordering::Relaxed) >= MAX_WIDE_LINES {
            return;
        }
        // SAFETY: main thread. Virtual index of the Backstep hold slot (first AreKeysDown entry), if any.
        let layers = unsafe { crate::pad::digital_slots(UserInputKey::Backstep) }
            .first()
            .map(|&(_, index, _)| unsafe { crate::pad::layers(index as usize) })
            .unwrap_or_default();
        let q = &ar.action_request_queue;
        let entry = q.input_entries.first().map(|e| (e.tae_id, bits(e.actions)));
        let mflags: u32 = {
            // SAFETY: MovementRequestFlags is a one-field bitfield over a u32.
            unsafe { std::mem::transmute(ar.movement_request_flags) }
        };
        crate::info!(
            "probe",
            "frame={frame} wide: req={:#x} new={:#x} rel={:#x} prev={:#x} rb_new={:#x} queued={:#x} rb_queued={:#x} cancel_ready={:#x}              polls={polls:#x} layers[{layers}] mv_flags={mflags:#x} mv_dur={:.3} roll_t={:.3} queue_mode={} q_tae={} q_n={} q0={entry:x?}              anim={anim}",
            bits(ar.action_requests),
            bits(ar.new_action_presses),
            bits(ar.released_actions),
            bits(ar.previous_action_requests),
            bits(ar.readback_new_presses),
            bits(ar.queued_action_inputs),
            bits(ar.readback_queued_inputs),
            bits(ar.cancel_ready_actions),
            ar.movement_request_duration,
            ar.action_timers.roll,
            ar.queue_mode_enabled as u8,
            q.current_tae_id,
            q.input_entries.len(),
        );
    }
}
