//! POSE-PLAN step 4: the player's skeleton pose → PoseState, every frame (ChrIns_PostPhysics).
//!
//! Where the pose lives: docs/research/elden-ring-pose.md (ChrIns+0x398 importer → +0x48 hkaSkeleton, +0x60 model pose).
//! Once per skeleton: map each PoseBone to an ER bone by name, build the bind pose in model space from the skeleton's reference
//! pose, and measure the ER → Skyrim basis on it (right = R_Thigh − L_Thigh, forward = toes − feet, up = model +Y).
//! Every frame: one read of the model pose, per bone `delta = q_model · q_bind⁻¹` in Skyrim's basis (protocol `rig`), the pelvis
//! offset from bind, ER's yaw, and `Active` while a dodge animation plays. Reads go through ReadProcessMemory (pose.rs helpers),
//! so a moved or freed pose is a failed read, never a crash.

use std::sync::atomic::Ordering;
use std::time::Instant;

use skyrimxer_protocol::proto::{OFF_SLOT_POSE, OFF_SLOT_POSE_BIND, POSE_BONE_COUNT, PoseBind, PoseBone, PoseFlag, PoseState};
use skyrimxer_protocol::{now_ms, now_us};
use skyrimxer_protocol::rig::{self, Basis, Quat, Vec3};
use skyrimxer_protocol::slot::SlotWriter;

use crate::pose::{IMPORTER, POSE_MODEL, POSE_SKELETON, f32s, hk_array, plausible, read_bytes, read_name, read_u64, u64_at};
use crate::{bridge, game, park};

const N: usize = POSE_BONE_COUNT as usize;
/// hkQsTransform: translation f32[4], rotation (x, y, z, w), scale f32[4].
const QS: usize = 48;
/// After a failed skeleton lookup, try again this many frames later (the model may still be loading).
const RETRY_FRAMES: u64 = 120;

/// ER bone per PoseBone (same order as the enum; c0000 names from the step 1 probe).
const ER_NAMES: [&str; N] = [
    "Pelvis", "Spine", "Spine1", "Spine2", "Neck", "Head", "L_Clavicle", "L_UpperArm", "L_Forearm", "L_Hand", "R_Clavicle", "R_UpperArm",
    "R_Forearm", "R_Hand", "L_Thigh", "L_Calf", "L_Foot", "R_Thigh", "R_Calf", "R_Foot", "L_UpArmTwist", "L_UpArmTwist1", "R_UpArmTwist",
    "R_UpArmTwist1",
];
/// Bone each PoseBone points at in bind (its segment): PoseBind directions, which Skyrim fits its own bind to ("" = none: the
/// parent's fit; twist bones share the upper arm's).
const SEGMENT_TO: [&str; N] = [
    "Spine", "Spine1", "Spine2", "Neck", "Head", "", "L_UpperArm", "L_Forearm", "L_Hand", "L_Finger2", "R_UpperArm", "R_Forearm", "R_Hand",
    "R_Finger2", "L_Calf", "L_Foot", "L_Toe0", "R_Calf", "R_Foot", "R_Toe0", "", "", "", "",
];

fn quat(f: &[f32]) -> Quat {
    [f[4], f[5], f[6], f[7]]
}

fn pos(f: &[f32]) -> Vec3 {
    [f[0], f[1], f[2]]
}

fn sub(a: Vec3, b: Vec3) -> Vec3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn mid(a: Vec3, b: Vec3) -> Vec3 {
    [(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5]
}

fn fmt(v: Vec3) -> String {
    format!("({:.3},{:.3},{:.3})", v[0], v[1], v[2])
}

/// The importer's skeleton pointer, from ChrIns.
fn skeleton(chr: usize) -> Option<(usize, usize)> {
    let importer = read_u64(chr + IMPORTER)? as usize;
    let skel = read_u64(importer + POSE_SKELETON)? as usize;
    (plausible(importer) && plausible(skel)).then_some((importer, skel))
}

/// What one skeleton needs to be streamed: the bone map, the bind pose of the mapped bones and the measured basis.
struct Rig {
    skeleton: usize,
    bones: usize,
    index: [usize; N],
    bind: [Quat; N],
    pelvis_bind: Vec3,
    basis: Basis,
    /// PoseBind directions (Skyrim basis, unit; zero = none).
    dirs: [Vec3; N],
}

impl Rig {
    fn resolve(skel: usize) -> Result<Self, String> {
        let (names_ptr, n) = hk_array(skel + 0x30).ok_or("bone array unreadable")?;
        let (parents_ptr, np) = hk_array(skel + 0x20).ok_or("parent array unreadable")?;
        let (ref_ptr, nr) = hk_array(skel + 0x40).ok_or("reference pose unreadable")?;
        if n == 0 || n > 1024 || np != n || nr != n {
            return Err(format!("sizes differ: bones={n} parents={np} reference={nr}"));
        }
        let raw = read_bytes(names_ptr, n * 16).ok_or("bone names unreadable")?;
        let names: Vec<String> = (0..n).map(|i| read_name(u64_at(&raw, i * 16) as usize).unwrap_or_default()).collect();
        let raw = read_bytes(parents_ptr, n * 2).ok_or("parents unreadable")?;
        let parents: Vec<i16> = raw.chunks_exact(2).map(|c| i16::from_le_bytes([c[0], c[1]])).collect();
        let reference = f32s(&read_bytes(ref_ptr, n * QS).ok_or("reference pose unreadable")?);

        // Bind pose in model space: parents come before children (checked by the step 1 probe; checked again here).
        let mut model: Vec<(Quat, Vec3)> = Vec::with_capacity(n);
        for i in 0..n {
            let f = &reference[i * 12..i * 12 + 12];
            let local = (rig::normalize(quat(f)).ok_or_else(|| format!("bind rotation of bone {i} is not a rotation"))?, pos(f));
            model.push(match parents[i] {
                p if p < 0 => local,
                p if (p as usize) < i => rig::compose(model[p as usize], local),
                p => return Err(format!("bone {i} has parent {p} after it")),
            });
        }
        let find = |name: &str| names.iter().position(|b| b == name);
        let at = |name: &str| find(name).map(|i| model[i].1).ok_or_else(|| format!("no bone {name}"));

        let mut index = [0usize; N];
        let mut bind = [rig::IDENTITY; N];
        for (k, name) in ER_NAMES.iter().enumerate() {
            index[k] = find(name).ok_or_else(|| format!("no bone {name}"))?;
            bind[k] = model[index[k]].0;
        }
        let feet = mid(at("L_Foot")?, at("R_Foot")?);
        let right = sub(at("R_Thigh")?, at("L_Thigh")?);
        let forward = sub(mid(at("L_Toe0")?, at("R_Toe0")?), feet);
        // Model space is Y-up (step 1). Head − feet leans ~2° back in bind, so it only checks the axis instead of defining it.
        let up = sub(at("Head")?, feet);
        let lean = (up[1] / (up[0] * up[0] + up[1] * up[1] + up[2] * up[2]).sqrt()).clamp(-1.0, 1.0).acos().to_degrees();
        if lean > 15.0 {
            return Err(format!("head − feet is {lean:.0} deg off model +Y; is this the Y-up model space?"));
        }
        let basis = Basis::from_directions(right, forward, [0.0, 1.0, 0.0]).ok_or("degenerate basis")?;
        let pelvis_bind = model[index[PoseBone::Pelvis as usize]].1;

        crate::info!("pose", "rig: {n} bones, all {N} PoseBones mapped ({})", {
            let list: Vec<String> = ER_NAMES.iter().zip(index).map(|(name, i)| format!("{name}={i}")).collect();
            list.join(" ")
        });
        crate::info!("pose", "ER bind dirs: right={} forward={} head-feet={} (lean {lean:.1} deg)", fmt(right), fmt(forward), fmt(up));
        crate::info!(
            "pose",
            "basis rows (Skyrim x/y/z in ER coords): {} {} {} det={:.3}",
            fmt(basis.rows[0]),
            fmt(basis.rows[1]),
            fmt(basis.rows[2]),
            basis.det()
        );
        let sky = |name: &str| at(name).map(|p| fmt(basis.vec(sub(p, feet)))).unwrap_or_default();
        crate::info!(
            "pose",
            "bind (Skyrim basis, m from mid-feet): Pelvis={} Head={} L_Thigh={} R_Thigh={} L_Hand={} R_Hand={}",
            sky("Pelvis"),
            sky("Head"),
            sky("L_Thigh"),
            sky("R_Thigh"),
            sky("L_Hand"),
            sky("R_Hand")
        );
        // Segment directions in Skyrim's basis: Skyrim's applier aligns its own bind segments to these (POSE-PLAN risk 2).
        let dirs: Vec<String> = ER_NAMES
            .iter()
            .zip(SEGMENT_TO)
            .filter(|(_, to)| !to.is_empty())
            .filter_map(|(from, to)| {
                let d = basis.vec(sub(at(to).ok()?, at(from).ok()?));
                let l = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
                Some(format!("{from}={}", fmt([d[0] / l, d[1] / l, d[2] / l])))
            })
            .collect();
        crate::info!("pose", "bind dir (Skyrim basis, unit): {}", dirs.join(" "));
        let mut seg = [[0.0f32; 3]; N];
        for (k, (from, to)) in ER_NAMES.iter().zip(SEGMENT_TO).enumerate() {
            if let (false, Ok(a), Ok(b)) = (to.is_empty(), at(from), at(to)) {
                let d = basis.vec(sub(b, a));
                let l = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
                if l > 1e-6 {
                    seg[k] = [d[0] / l, d[1] / l, d[2] / l];
                }
            }
        }
        // Which ER twist bone sits nearer the shoulder decides the Twist1/Twist2 mapping (logged; the order above is a guess).
        let twist: Vec<String> = ["L_UpperArm", "L_UpArmTwist", "L_UpArmTwist1", "L_Forearm"]
            .iter()
            .filter_map(|name| Some(format!("{name}={}", fmt(basis.vec(sub(at(name).ok()?, feet))))))
            .collect();
        crate::info!("pose", "left arm twist chain bind (Skyrim basis, m from mid-feet): {}", twist.join(" "));
        Ok(Self { skeleton: skel, bones: n, index, bind, pelvis_bind, basis, dirs: seg })
    }

    /// This frame's pose from the model-pose array, or why not.
    fn sample(&self, importer: usize) -> Result<PoseState, &'static str> {
        let (ptr, n) = hk_array(importer + POSE_MODEL).ok_or("model pose unreadable")?;
        if n != self.bones {
            return Err("model pose size changed");
        }
        let last = self.index.iter().max().copied().unwrap_or(0) + 1;
        let f = f32s(&read_bytes(ptr, last * QS).ok_or("model pose unreadable")?);
        let mut pose = PoseState { bone_count: POSE_BONE_COUNT, ..Default::default() };
        for k in 0..N {
            let b = &f[self.index[k] * 12..self.index[k] * 12 + 12];
            let q = rig::normalize(quat(b)).ok_or("bone rotation not finite")?;
            pose.rot[k * 4..k * 4 + 4].copy_from_slice(&self.basis.rot(rig::delta(q, self.bind[k])));
        }
        let p = self.index[PoseBone::Pelvis as usize] * 12;
        pose.pelvis_offset = self.basis.vec(sub(pos(&f[p..p + 12]), self.pelvis_bind));
        if pose.pelvis_offset.iter().any(|v| !v.is_finite()) {
            return Err("pelvis offset not finite");
        }
        Ok(pose)
    }
}

/// Per Active stretch, for the `[pose] Active off` line.
#[derive(Default)]
struct Stats {
    frames: u32,
    pelvis_turn: f32,
    pelvis_drop: f32,
    write_us: u128,
}

pub struct PoseStream {
    writer: Option<SlotWriter<PoseState>>,
    bind_writer: Option<SlotWriter<PoseBind>>,
    /// The rig's skeleton whose PoseBind was last written (0 = none yet).
    bind_written: usize,
    rig: Option<Rig>,
    retry_at: u64,
    active: bool,
    stats: Stats,
    /// Last error logged; repeats are counted, not logged, until a pose works again.
    error: Option<String>,
    repeats: u32,
}

impl PoseStream {
    pub fn new() -> Self {
        Self { writer: None, bind_writer: None, bind_written: 0, rig: None, retry_at: 0, active: false, stats: Stats::default(), error: None, repeats: 0 }
    }

    pub fn run(&mut self) {
        let started = Instant::now();
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        if self.writer.is_none() {
            if let Some(r) = bridge::shared().and_then(|s| s.region()) {
                self.writer = Some(SlotWriter::new(r.clone(), OFF_SLOT_POSE));
                self.bind_writer = Some(SlotWriter::new(r, OFF_SLOT_POSE_BIND));
            }
        }
        if self.writer.is_none() {
            return;
        }
        // SAFETY: task callbacks run on the game's main thread.
        let player = unsafe { game::main_player() };
        let mut pose = PoseState { bone_count: POSE_BONE_COUNT, ..Default::default() };
        for r in pose.rot.chunks_exact_mut(4) {
            r.copy_from_slice(&rig::IDENTITY);
        }
        let mut anim = -1;
        if let Some(player) = player {
            let snap = game::snapshot(player);
            anim = snap.anim_id;
            match self.sample(&player.chr_ins as *const _ as usize, frame) {
                Ok(p) => {
                    pose = p;
                    if let Some(e) = self.error.take() {
                        crate::info!("pose", "pose readable again (after \"{e}\" x{})", self.repeats + 1);
                    }
                }
                Err(e) if self.error.as_ref() == Some(&e) => self.repeats += 1,
                Err(e) => {
                    crate::warn!("pose", "no pose: {e} (repeats are counted, not logged)");
                    self.error = Some(e);
                    self.repeats = 0;
                }
            }
            pose.yaw = snap.yaw;
            if self.rig.is_some() && park::is_dodge_anim(anim) {
                pose.flags = 1 << PoseFlag::Active as u32;
            }
        }
        // No player / no pose: flags 0 (Skyrim plays its own animation), still written so it isn't stale.
        if let (Some(rig), Some(w)) = (self.rig.as_ref(), self.bind_writer.as_mut())
            && rig.skeleton != self.bind_written
        {
            let mut bind = PoseBind { bone_count: POSE_BONE_COUNT, frame, ..Default::default() };
            for (k, d) in rig.dirs.iter().enumerate() {
                bind.dir[k * 3..k * 3 + 3].copy_from_slice(d);
            }
            w.write(&bind);
            self.bind_written = rig.skeleton;
            crate::info!("pose", "PoseBind written (skeleton {:#x}, er_frame={frame})", rig.skeleton);
        }
        pose.frame = frame;
        pose.time_ms = now_ms();
        pose.time_us = now_us();
        let active = pose.flags != 0;
        if active && !self.active {
            crate::info!("pose", "Active on anim={anim} er_frame={frame}");
            self.stats = Stats::default();
        }
        if active {
            let s = &mut self.stats;
            s.frames += 1;
            s.pelvis_turn = s.pelvis_turn.max(rig::angle(pose.rot[0..4].try_into().unwrap()).to_degrees());
            s.pelvis_drop = s.pelvis_drop.max(-pose.pelvis_offset[2]);
        }
        if !active && self.active {
            let s = &self.stats;
            crate::info!(
                "pose",
                "Active off er_frame={frame} (frames={} pelvis turn max={:.0} deg, pelvis drop max={:.2} m, write max={} us)",
                s.frames,
                s.pelvis_turn,
                s.pelvis_drop,
                s.write_us
            );
        }
        self.active = active;
        self.writer.as_mut().unwrap().write(&pose);
        self.stats.write_us = self.stats.write_us.max(started.elapsed().as_micros());
    }

    fn sample(&mut self, chr: usize, frame: u64) -> Result<PoseState, String> {
        let (importer, skel) = skeleton(chr).ok_or("pose importer unreadable")?;
        if self.rig.as_ref().is_none_or(|r| r.skeleton != skel) {
            if frame < self.retry_at {
                return Err("waiting to retry the skeleton".into());
            }
            self.rig = None;
            match Rig::resolve(skel) {
                Ok(rig) => self.rig = Some(rig),
                Err(e) => {
                    self.retry_at = frame + RETRY_FRAMES;
                    return Err(format!("skeleton not usable: {e}"));
                }
            }
        }
        Ok(self.rig.as_ref().unwrap().sample(importer)?)
    }
}
