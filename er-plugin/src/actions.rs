//! Action injection: ER performs actions because we hold its in-game keys on the virtual pad (pad.rs). ER's own pipeline and gating
//! (stamina, recovery, tap = roll on release / hold = dash) still decide what happens. Writing `action_requests` directly does NOT
//! work: the engine rebuilds it from the pad every ChrIns_PreBehavior (probe result, docs/research/elden-ring-input.md).
//!
//! P3 step 2 self-test (`selftest=dodge` in skyrimxer_er.cfg): every PULSE_EVERY frames hold Backstep for the next length in
//! HOLD_FRAMES, with BackstepTapped on the press frame only (what a real tap does), then release it, and watch the player for
//! WATCH_FRAMES frames from a ChrIns_PostPhysics task, logging every change. `selftest=roll` (P3 step 5) also holds a move direction
//! on the virtual analog stick around each press, so ER rolls instead of backstepping.

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
    matches!(crate::config::get().selftest.as_str(), "dodge" | "roll")
}

/// `selftest=roll`: each pulse also holds a move direction (cycled per pulse) from MOVE_LEAD frames before the press until
/// MOVE_TAIL frames after the release, so ER rolls instead of backstepping (P3 step 5).
fn selftest_roll() -> bool {
    crate::config::get().selftest == "roll"
}
const MOVE_LEAD: u32 = 10;
const MOVE_TAIL: u32 = 5;
/// (x right, y forward, name) per pulse.
const DIRECTIONS: [(f32, f32, &str); 4] = [(0.0, 1.0, "forward"), (1.0, 0.0, "right"), (0.0, -1.0, "back"), (-1.0, 0.0, "left")];
/// Frames left holding the self-test direction (read by the watcher to log the polled analog values).
static MOVE_LEFT: AtomicU32 = AtomicU32::new(0);

/// Starts a WATCH_FRAMES watch window from `frame` (remote.rs calls this on each Dodge press from Skyrim).
pub fn watch(frame: u64) {
    WATCH_LEFT.store(WATCH_FRAMES, Ordering::Relaxed);
    INJECT_FRAME.store(frame, Ordering::Relaxed);
}

/// Cap for the word-diff lines (i-frame search), per session.
const MAX_FLAG_DIFF_LINES: u32 = 400;
static FLAG_DIFF_LINES: AtomicU32 = AtomicU32::new(0);

/// `bytes` bytes at `ptr` as u32 words (unaligned-safe). Bounded probe for the i-frame search: the eldenring-rs
/// `perfect_invincibility`/`dodging` bits read 0 on 2.7.1, so the Watcher diffs whole regions frame to frame.
///
/// # Safety
/// `ptr..ptr+bytes` must be readable game memory; main thread only (task callback).
unsafe fn raw_words(ptr: *const u8, bytes: usize) -> Vec<u32> {
    (0..bytes / 4).map(|i| unsafe { std::ptr::read_unaligned(ptr.add(i * 4).cast::<u32>()) }).collect()
}

/// The regions the Watcher diffs: (name, words to skip at the start (vtable/owner pointers), words). `CSChrEventModule` and the
/// ChrIns flag bytes were diffed in attempt 2 and ruled out (docs/research/elden-ring-state.md).
fn probe_regions(player: &eldenring::cs::PlayerIns) -> [(&'static str, usize, Vec<u32>); 1] {
    use eldenring::cs::CSChrActionFlagModule;
    let flag: &CSChrActionFlagModule = &player.chr_ins.modules.action_flag;
    // SAFETY: the region is a live object's own bytes.
    unsafe { [("action_flag", 4, raw_words((flag as *const CSChrActionFlagModule).cast(), size_of::<CSChrActionFlagModule>()))] }
}

/// Runs in `inject_group()`. Self-test only for now; P3 step 4 feeds it from Skyrim's InputState.
pub struct Injector {
    frames: u64,
    dir: (f32, f32, &'static str),
}

impl Injector {
    pub fn new() -> Self {
        Self { frames: 0, dir: DIRECTIONS[0] }
    }

    pub fn run(&mut self) {
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.frames = 0;
            return;
        };
        self.frames += 1;
        let frame = crate::bridge::FRAMES.load(Ordering::Relaxed);
        let next = PULSES.load(Ordering::Relaxed) as usize;
        if selftest_roll() && self.frames % PULSE_EVERY == PULSE_EVERY - MOVE_LEAD as u64 && PULSE_LEFT.load(Ordering::Relaxed) == 0 {
            // The pulse after this one: same hold length the next block picks.
            let hold = HOLD_FRAMES[next % HOLD_FRAMES.len()];
            self.dir = DIRECTIONS[next % DIRECTIONS.len()];
            // SAFETY: main thread.
            let slots = unsafe { [UserInputKey::MoveForwards, UserInputKey::MoveRight].map(|k| pad::analog_slots(k)) };
            crate::info!("action", "frame={frame} selftest: hold move {} for {MOVE_LEAD} frames before the press; analog slots F/R={slots:?}", self.dir.2);
            MOVE_LEFT.store(MOVE_LEAD + hold + 1 + MOVE_TAIL, Ordering::Relaxed);
        }
        let move_left = MOVE_LEFT.load(Ordering::Relaxed);
        if move_left > 0 {
            let (x, y) = if move_left > 1 { (self.dir.0, self.dir.1) } else { (0.0, 0.0) };
            // SAFETY: main thread. Held while move_left > 1; the last frame writes the release.
            unsafe { pad::set_move(x, y) };
            MOVE_LEFT.store(move_left - 1, Ordering::Relaxed);
        }
        if self.frames % PULSE_EVERY == 0 && PULSE_LEFT.load(Ordering::Relaxed) == 0 {
            let n = PULSES.fetch_add(1, Ordering::Relaxed) + 1;
            let s = game::snapshot(player);
            // SAFETY: main thread.
            let slots = unsafe { pad::digital_slots(UserInputKey::Backstep) };
            let hold = HOLD_FRAMES[(n as usize - 1) % HOLD_FRAMES.len()];
            let dir = if selftest_roll() { self.dir.2 } else { "none" };
            crate::info!(
                "action",
                "frame={frame} selftest pulse #{n}: hold Backstep {hold} frames then release (move {dir}); before: stamina={} anim={} \
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

/// LOCO-PLAN stage B probe (`selftest=walk` / `selftest=sprint`): a fixed script of held stick values (and Dodge holds) on the virtual
/// pad, no Skyrim involved. Every SAMPLE_EVERY frames it logs the speed of the virtual position (park.rs keeps the character pinned),
/// the player's yaw, the camera's yaw and the anim id; each step ends with a `[loco-test] step` summary (mean speed of its second half).
pub fn locotest_enabled() -> bool {
    matches!(crate::config::get().selftest.as_str(), "walk" | "sprint")
}

/// (x right, y forward, Dodge: 0 = off, 1 = tap (press frame + LOCO_TAP_FRAMES), 2 = held, frames, name)
type LocoStep = (f32, f32, u8, u32, &'static str);
const LOCO_WALK: [LocoStep; 8] = [
    (0.0, 0.0, 0, 60, "idle"),
    (0.0, 0.3, 0, 150, "fwd 0.3"),
    (0.0, 0.6, 0, 150, "fwd 0.6"),
    (0.0, 1.0, 0, 150, "fwd 1.0"),
    (0.0, 0.0, 0, 60, "stop"),
    (1.0, 0.0, 0, 150, "right 1.0"),
    (0.0, -1.0, 0, 150, "back 1.0"),
    (0.0, 0.0, 0, 60, "stop"),
];
const LOCO_SPRINT: [LocoStep; 6] = [
    (0.0, 0.0, 0, 60, "idle"),
    (0.0, 1.0, 0, 90, "run"),
    (0.0, 1.0, 1, 90, "run + dodge tap"),
    (0.0, 1.0, 2, 240, "run + dodge held"),
    (0.0, 1.0, 0, 60, "run after"),
    (0.0, 0.0, 0, 90, "stop"),
];
const LOCO_TAP_FRAMES: u32 = 6;
const LOCO_WAIT_FRAMES: u64 = 180;
const SAMPLE_EVERY: u32 = 10;

pub struct LocoTest {
    in_world: u64,
    step: usize,
    step_frame: u32,
    last_pos: Option<[f32; 3]>,
    /// Second-half distance (m) and frames of the current step, for the summary.
    dist: f32,
    dist_frames: u32,
    anims: Vec<i32>,
    done: bool,
}

impl LocoTest {
    pub fn new() -> Self {
        Self { in_world: 0, step: 0, step_frame: 0, last_pos: None, dist: 0.0, dist_frames: 0, anims: Vec::new(), done: false }
    }

    fn script() -> &'static [LocoStep] {
        if crate::config::get().selftest == "sprint" { &LOCO_SPRINT } else { &LOCO_WALK }
    }

    pub fn run(&mut self) {
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.in_world = 0;
            return;
        };
        self.in_world += 1;
        if self.in_world < LOCO_WAIT_FRAMES || self.done {
            return;
        }
        let frame = crate::bridge::FRAMES.load(Ordering::Relaxed);
        let script = Self::script();
        let (x, y, dodge, frames, name) = script[self.step];
        if self.step_frame == 0 {
            let s = game::snapshot(player);
            crate::info!("loco-test", "frame={frame} step {} \"{name}\": stick x={x} y={y} dodge={dodge} for {frames} frames; stamina={} anim={}", self.step, s.stamina, s.anim_id);
            self.dist = 0.0;
            self.dist_frames = 0;
            self.anims.clear();
        }
        // SAFETY (pad calls): main thread, in the inject group (after PadStep, before the characters read input).
        unsafe { pad::set_move(x, y) };
        let (held, tapped) = match dodge {
            1 => (self.step_frame <= LOCO_TAP_FRAMES, self.step_frame == 0),
            2 => (true, self.step_frame == 0),
            _ => (false, false),
        };
        unsafe {
            pad::set_digital(UserInputKey::Backstep, held);
            pad::set_digital(UserInputKey::BackstepTapped, tapped);
        }
        let s = game::snapshot(player);
        if self.anims.last() != Some(&s.anim_id) {
            self.anims.push(s.anim_id);
        }
        // Virtual position = last ChrIns_PostPhysics (park.rs): one frame behind, fine for speeds.
        let pos = crate::park::virtual_pos();
        if let (Some(p), Some(l)) = (pos, self.last_pos) {
            let d = (p[0] - l[0]).hypot(p[2] - l[2]);
            if d < 1.5 && self.step_frame >= frames / 2 {
                self.dist += d;
                self.dist_frames += 1;
            }
        }
        if self.step_frame % SAMPLE_EVERY == 0 {
            // SAFETY: main thread.
            let cam = unsafe { game::camera_yaw() };
            let rel = cam.map(|c| (s.yaw - c + std::f32::consts::PI).rem_euclid(std::f32::consts::TAU) - std::f32::consts::PI);
            let speed = match (pos, self.last_pos) {
                (Some(p), Some(l)) => (p[0] - l[0]).hypot(p[2] - l[2]) * 60.0,
                _ => 0.0,
            };
            crate::info!(
                "loco-test",
                "frame={frame} step {} +{} speed={speed:.2} m/s yaw={:.3} cam={} yaw-cam={} anim={} stamina={} real=({:.2},{:.2}) virt={}",
                self.step,
                self.step_frame,
                s.yaw,
                cam.map_or("none".into(), |c| format!("{c:.3}")),
                rel.map_or("none".into(), |r| format!("{:.0}deg", r.to_degrees())),
                s.anim_id,
                s.stamina,
                s.pos[0],
                s.pos[2],
                pos.map_or("none".into(), |p| format!("({:.2},{:.2})", p[0], p[2]))
            );
        }
        self.last_pos = pos;
        self.step_frame += 1;
        if self.step_frame >= frames {
            let mean = if self.dist_frames > 0 { self.dist / self.dist_frames as f32 * 60.0 } else { 0.0 };
            crate::info!("loco-test", "frame={frame} step {} \"{name}\" done: mean speed (2nd half) {mean:.2} m/s, anims {:?}, stamina={}", self.step, self.anims, s.stamina);
            self.step += 1;
            self.step_frame = 0;
            if self.step >= script.len() {
                self.done = true;
                // SAFETY: main thread. Release everything.
                unsafe {
                    pad::set_move(0.0, 0.0);
                    pad::set_digital(UserInputKey::Backstep, false);
                    pad::set_digital(UserInputKey::BackstepTapped, false);
                }
                crate::info!("loco-test", "frame={frame} script done");
            }
        }
    }
}

/// Runs in ChrIns_PostPhysics (after behavior): logs what the injected press did, and which probed words changed.
pub struct Watcher {
    last: Option<game::Snapshot>,
    last_words: Option<[(&'static str, usize, Vec<u32>); 1]>,
    last_ev_flags: u8,
    hits: HitLog,
}

/// Cap for the hit lines (i-frame timing test), per session.
const MAX_HIT_LINES: u32 = 300;
static HIT_LINES: AtomicU32 = AtomicU32::new(0);

/// I-frame timing test: opens a watch window on every dodge animation the player starts (real presses too, not only injected ones),
/// and logs every HP loss with the modifier bits and how far into the last dodge it landed. Hits that never land while a bit is set =
/// that bit is the i-frame window.
#[derive(Default)]
struct HitLog {
    last_anim: i32,
    last_hp: i32,
    /// (frame, anim) of the last dodge-family animation start (anim id 27xxx without the group prefix: backstep 27010, rolls 271xx).
    dodge: Option<(u64, i32)>,
}

impl HitLog {
    fn run(&mut self, player: &eldenring::cs::PlayerIns, s: &game::Snapshot) {
        let frame = crate::bridge::FRAMES.load(Ordering::Relaxed);
        if s.anim_id != self.last_anim && (s.anim_id % 1_000_000) / 1000 == 27 {
            self.dodge = Some((frame, s.anim_id));
            if WATCH_LEFT.load(Ordering::Relaxed) == 0 {
                watch(frame);
                crate::info!("action", "frame={frame} dodge anim {} started: watch window opened", s.anim_id);
            }
        }
        if s.hp < self.last_hp && s.hp > 0 && HIT_LINES.fetch_add(1, Ordering::Relaxed) < MAX_HIT_LINES {
            // SAFETY: ChrActionModifiersFlags is a one-field bitfield over a u64.
            let mods: u64 = unsafe { std::mem::transmute(player.chr_ins.modules.action_flag.action_modifiers_flags) };
            let dodge = self.dodge.map_or("none".to_string(), |(f, a)| format!("{a} +{}", frame - f));
            crate::info!(
                "probe",
                "frame={frame} HIT hp {}→{} anim={} mods={mods:#x} dodging={} last_dodge={dodge}",
                self.last_hp,
                s.hp,
                s.anim_id,
                s.dodging as u8
            );
        }
        self.last_anim = s.anim_id;
        self.last_hp = s.hp;
    }
}

impl Watcher {
    pub fn new() -> Self {
        Self { last: None, last_words: None, last_ev_flags: 0, hits: HitLog::default() }
    }

    pub fn run(&mut self) {
        // SAFETY: main thread.
        if let Some(player) = unsafe { game::main_player() } {
            let s = game::snapshot(player);
            self.hits.run(player, &s);
        }
        let left = WATCH_LEFT.load(Ordering::Relaxed);
        if left == 0 {
            self.last = None;
            self.last_words = None;
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
        // eldenring-rs: "bit in pos 1 is iframes" (CSChrEventModule.flags), unverified: logged raw next to anim/stamina.
        let ev_flags = player.chr_ins.modules.event.flags;
        let changed = match &self.last {
            None => true,
            Some(l) => {
                l.anim_id != s.anim_id
                    || l.iframe != s.iframe
                    || l.dodging != s.dodging
                    || l.stamina != s.stamina
                    || l.hp != s.hp
                    || ev_flags != self.last_ev_flags
            }
        };
        self.last_ev_flags = ev_flags;
        if changed || pressing {
            crate::info!(
                "action",
                "frame={frame} +{since}: hp={} stamina={} anim={} iframe={} dodging={} ev_flags={ev_flags:#04x} | requests.sp_move={} \
                 new_presses.sp_move={} readback_new.sp_move={} roll_timer={:.3} mv(F/B/L/R)={}",
                s.hp,
                s.stamina,
                s.anim_id,
                s.iframe as u8,
                s.dodging as u8,
                ar.action_requests.sp_move() as u8,
                ar.new_action_presses.sp_move() as u8,
                ar.readback_new_presses.sp_move() as u8,
                ar.action_timers.roll,
                // SAFETY: main thread.
                unsafe { pad::move_polls() }
            );
        }
        let regions = probe_regions(player);
        if let Some(prev) = &self.last_words {
            for ((name, skip, old), (_, _, new)) in prev.iter().zip(&regions) {
                for (i, (o, n)) in old.iter().zip(new).enumerate().skip(*skip) {
                    if o != n && FLAG_DIFF_LINES.fetch_add(1, Ordering::Relaxed) < MAX_FLAG_DIFF_LINES {
                        crate::info!("probe", "frame={frame} +{since}: {name} +{:#05x}: {o:#010x}→{n:#010x} anim={}", i * 4, s.anim_id);
                    }
                }
            }
        }
        self.last_words = Some(regions);
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

