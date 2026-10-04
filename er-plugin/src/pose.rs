//! Research probe for docs/POSE-PLAN.md step 1: where does ER keep the player's skeleton pose? `pose_probe=1` (dev.ps1 -ErPoseProbe).
//!
//! Once, a few seconds after the player spawns, this walks the object graph that starts at the opaque pose pointers on ChrIns, ChrCtrl
//! and the model item. Every read is ReadProcessMemory on our own process, so a bad pointer is a failed read, never a crash. Each object
//! is named by its MSVC RTTI. The walk looks for (a) arrays of transforms (hkQsTransform 48 B, 3x4 or 4x4 matrices) and (b) arrays of
//! bone-name strings. After that, the transform candidates are sampled every few frames, logging how far their bones turn (a roll should
//! turn many bones a lot, idle only a little). Output: `[pose]` log lines + logs/pose_probe.txt (object tree, all names).

use std::collections::HashSet;
use std::ffi::c_void;
use std::fmt::Write as _;
use std::sync::atomic::Ordering;

use windows::Win32::System::Diagnostics::Debug::ReadProcessMemory;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::Threading::GetCurrentProcess;

use crate::{bridge, game};

/// Frames after spawn before the walk (let the character finish loading).
const WALK_AFTER_FRAMES: u64 = 300;
const MAX_DEPTH: usize = 4;
const MAX_OBJECTS: usize = 4000;
const OBJECT_BYTES: [usize; 4] = [0x400, 0x200, 0x100, 0x40];
const MIN_RECORDS: usize = 16;
const MAX_RECORDS: usize = 1000;
const MAX_SAMPLED: usize = 12;
const SAMPLE_EVERY: u64 = 4;
const LOG_EVERY: u64 = 30;
/// Stop sampling after this many log lines (≈ 50 s at 60 fps).
const MAX_LOG_LINES: u32 = 100;

// ---- safe memory access --------------------------------------------------------------------------------------------

fn plausible(addr: usize) -> bool {
    addr > 0x10000 && addr < 0x7FFF_FFFF_0000 && addr % 8 == 0
}

fn read_into(addr: usize, buf: &mut [u8]) -> bool {
    if addr < 0x10000 || addr > 0x7FFF_FFFF_FFFF {
        return false;
    }
    let mut n = 0usize;
    // SAFETY: ReadProcessMemory validates the source range itself and fails instead of faulting; buf is ours.
    let ok = unsafe {
        ReadProcessMemory(GetCurrentProcess(), addr as *const c_void, buf.as_mut_ptr().cast(), buf.len(), Some(&mut n)).is_ok()
    };
    ok && n == buf.len()
}

fn read_bytes(addr: usize, len: usize) -> Option<Vec<u8>> {
    let mut buf = vec![0u8; len];
    read_into(addr, &mut buf).then_some(buf)
}

fn read_u64(addr: usize) -> Option<u64> {
    let mut b = [0u8; 8];
    read_into(addr, &mut b).then(|| u64::from_le_bytes(b))
}

fn read_u32(addr: usize) -> Option<u32> {
    let mut b = [0u8; 4];
    read_into(addr, &mut b).then(|| u32::from_le_bytes(b))
}

fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

fn f32s(b: &[u8]) -> Vec<f32> {
    b.chunks_exact(4).map(|c| f32::from_le_bytes(c.try_into().unwrap())).collect()
}

/// A NUL-terminated name of 2..63 plain characters (letters, digits, `_`, space, `[]`, `.`, `-`).
fn read_name(addr: usize) -> Option<String> {
    if addr < 0x10000 || addr > 0x7FFF_FFFF_FFFF {
        return None;
    }
    let b = [64usize, 32, 16].iter().find_map(|&n| read_bytes(addr, n))?;
    let end = b.iter().position(|&c| c == 0)?;
    let s = &b[..end];
    let ok = s.len() >= 2 && s.iter().all(|&c| c.is_ascii_alphanumeric() || b"_ []().-".contains(&c));
    ok.then(|| String::from_utf8_lossy(s).into_owned())
}

/// MSVC x64 RTTI: vtable[-1] → CompleteObjectLocator (signature 1, +0xC type descriptor RVA, +0x14 own RVA) → name at TD+0x10.
fn rtti_name(obj: usize) -> Option<String> {
    let vt = read_u64(obj)? as usize;
    if !plausible(vt) {
        return None;
    }
    let col = read_u64(vt - 8)? as usize;
    if read_u32(col)? != 1 {
        return None;
    }
    let td_rva = read_u32(col + 0xC)? as usize;
    let base = col.checked_sub(read_u32(col + 0x14)? as usize)?;
    let raw = [128usize, 64, 32].iter().find_map(|&n| read_bytes(base + td_rva + 0x10, n))?;
    let end = raw.iter().position(|&c| c == 0)?;
    let name = String::from_utf8_lossy(&raw[..end]).into_owned();
    name.starts_with(".?A").then(|| name.trim_start_matches(".?AV").trim_start_matches(".?AU").trim_end_matches("@@").to_string())
}

/// eldenring.exe's image range: pointers into it (vtables, code, statics) are not followed.
fn image_range() -> (usize, usize) {
    // SAFETY: plain Win32 call; None = the exe.
    let Ok(h) = (unsafe { GetModuleHandleW(None) }) else { return (0, 0) };
    let base = h.0 as usize;
    let size = read_u32(base + 0x3C)
        .and_then(|lfanew| read_u32(base + lfanew as usize + 0x50))
        .unwrap_or(0) as usize;
    (base, base + size)
}

// ---- transform / name detection -----------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    /// hkQsTransform: translation vec4, rotation quat (x,y,z,w), scale vec4.
    Qs,
    /// 3x4: three rows of (r r r t).
    Rows34,
    /// 4x3: rotation rows (r r r) ×3, then translation.
    Rows43,
    /// 4x4 with rotation in the upper-left 3x3.
    M44,
}

impl Kind {
    fn stride(self) -> usize {
        match self {
            Kind::Qs | Kind::Rows34 | Kind::Rows43 => 48,
            Kind::M44 => 64,
        }
    }
}

type Mat3 = [[f32; 3]; 3];

fn quat_to_mat(q: [f32; 4]) -> Mat3 {
    let [x, y, z, w] = q;
    [
        [1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)],
        [2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)],
        [2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)],
    ]
}

fn orthonormal(m: &Mat3) -> bool {
    let dot = |a: &[f32; 3], b: &[f32; 3]| a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    (0..3).all(|i| (dot(&m[i], &m[i]) - 1.0).abs() < 0.03) && (0..3).all(|i| (i + 1..3).all(|j| dot(&m[i], &m[j]).abs() < 0.03))
}

/// Rotation of one record, or None if the record doesn't look like a `kind` transform.
fn record_rotation(f: &[f32], kind: Kind) -> Option<Mat3> {
    if f.iter().any(|v| !v.is_finite()) {
        return None;
    }
    let m = match kind {
        Kind::Qs => {
            let t = &f[0..3];
            let q = [f[4], f[5], f[6], f[7]];
            let len = (q.iter().map(|v| v * v).sum::<f32>()).sqrt();
            let scale_ok = f[8..11].iter().all(|s| (0.2..5.0).contains(s));
            if (len - 1.0).abs() > 0.01 || !scale_ok || t.iter().any(|v| v.abs() > 100.0) {
                return None;
            }
            quat_to_mat(q)
        }
        Kind::Rows34 => {
            if [f[3], f[7], f[11]].iter().any(|v| v.abs() > 5000.0) {
                return None;
            }
            [[f[0], f[1], f[2]], [f[4], f[5], f[6]], [f[8], f[9], f[10]]]
        }
        Kind::Rows43 => {
            if f[9..12].iter().any(|v| v.abs() > 5000.0) {
                return None;
            }
            [[f[0], f[1], f[2]], [f[3], f[4], f[5]], [f[6], f[7], f[8]]]
        }
        Kind::M44 => {
            let last_row = (f[12].abs() > 1e-3 || f[13].abs() > 1e-3 || f[14].abs() > 1e-3) && (f[15] - 1.0).abs() < 1e-3;
            let last_col = f[3].abs() < 1e-3 && f[7].abs() < 1e-3 && f[11].abs() < 1e-3 && (f[15] - 1.0).abs() < 1e-3;
            if !(last_row || last_col) {
                return None;
            }
            [[f[0], f[1], f[2]], [f[4], f[5], f[6]], [f[8], f[9], f[10]]]
        }
    };
    orthonormal(&m).then_some(m)
}

/// Rotation angle between two rotations, degrees (acos((tr(AᵀB) - 1) / 2)).
fn angle_deg(a: &Mat3, b: &Mat3) -> f32 {
    let mut tr = 0.0;
    for i in 0..3 {
        for j in 0..3 {
            tr += a[i][j] * b[i][j];
        }
    }
    (((tr - 1.0) / 2.0).clamp(-1.0, 1.0)).acos().to_degrees()
}

/// Leading valid records at `addr` (up to `max`), or 0. Reads in chunks of 16 records so a short array near a page end still counts.
fn count_records(addr: usize, kind: Kind, max: usize) -> usize {
    let stride = kind.stride();
    let mut n = 0;
    while n < max {
        let chunk = (max - n).min(16);
        let Some(b) = read_bytes(addr + n * stride, chunk * stride) else {
            // Fall back to single records at the edge of readable memory.
            match read_bytes(addr + n * stride, stride) {
                Some(b) if record_rotation(&f32s(&b), kind).is_some() => {
                    n += 1;
                    continue;
                }
                _ => break,
            }
        };
        let f = f32s(&b);
        for r in 0..chunk {
            if record_rotation(&f[r * stride / 4..(r + 1) * stride / 4], kind).is_none() {
                return n;
            }
            n += 1;
        }
    }
    n
}

/// Names at `addr` as an array of `stride`-byte records whose first qword points at a name string.
fn read_names(addr: usize, stride: usize, max: usize) -> Vec<String> {
    let mut names = Vec::new();
    while names.len() < max {
        let Some(p) = read_u64(addr + names.len() * stride) else { break };
        let Some(name) = read_name(p as usize) else { break };
        names.push(name);
    }
    names
}

// ---- the walk -----------------------------------------------------------------------------------------------------

#[derive(Clone, Debug)]
struct Candidate {
    path: String,
    addr: usize,
    kind: Kind,
    count: usize,
}

struct NameList {
    path: String,
    names: Vec<String>,
}

struct Walk {
    image: (usize, usize),
    visited: HashSet<usize>,
    tested: HashSet<usize>,
    tree: String,
    transforms: Vec<Candidate>,
    names: Vec<NameList>,
}

impl Walk {
    fn new() -> Self {
        Self {
            image: image_range(),
            visited: HashSet::new(),
            tested: HashSet::new(),
            tree: String::new(),
            transforms: Vec::new(),
            names: Vec::new(),
        }
    }

    fn in_image(&self, v: usize) -> bool {
        v >= self.image.0 && v < self.image.1
    }

    /// Test `addr` as the start of a transform array and of a name array. `exact` = hkArray count (else count what's valid).
    fn test_array(&mut self, path: &str, addr: usize, exact: Option<usize>) {
        if !self.tested.insert(addr) {
            return;
        }
        let max = exact.unwrap_or(MAX_RECORDS).min(MAX_RECORDS);
        for kind in [Kind::Qs, Kind::Rows34, Kind::Rows43, Kind::M44] {
            let n = count_records(addr, kind, max);
            let enough = match exact {
                Some(c) => n == c.min(MAX_RECORDS),
                None => n >= MIN_RECORDS,
            };
            if enough && n >= MIN_RECORDS {
                let _ = writeln!(self.tree, "    ** {path}: {n} x {kind:?} @ {addr:#x}");
                self.transforms.push(Candidate { path: path.to_string(), addr, kind, count: n });
                break;
            }
        }
        for stride in [8usize, 16] {
            let names = read_names(addr, stride, max);
            if names.len() >= MIN_RECORDS && exact.is_none_or(|c| names.len() == c.min(MAX_RECORDS)) {
                let _ = writeln!(self.tree, "    ** {path}: {} names (stride {stride}): {}", names.len(), names.join(", "));
                self.names.push(NameList { path: format!("{path} (stride {stride})"), names });
                break;
            }
        }
    }

    fn run(&mut self, roots: Vec<(String, usize)>) {
        let mut queue: std::collections::VecDeque<(String, usize, usize)> = roots.into_iter().map(|(p, a)| (p, a, 0)).collect();
        while let Some((path, obj, depth)) = queue.pop_front() {
            if self.visited.len() >= MAX_OBJECTS {
                let _ = writeln!(self.tree, "(object limit {MAX_OBJECTS} reached)");
                break;
            }
            if !plausible(obj) || self.in_image(obj) || !self.visited.insert(obj) {
                continue;
            }
            let Some(block) = OBJECT_BYTES.iter().find_map(|&n| read_bytes(obj, n)) else { continue };
            let class = rtti_name(obj).unwrap_or_default();
            let _ = writeln!(self.tree, "{}{path} @ {obj:#x} [{}] {class}", "  ".repeat(depth), block.len());
            self.test_array(&path, obj, None);
            for o in (0..block.len().saturating_sub(8)).step_by(8) {
                let v = u64_at(&block, o) as usize;
                if !plausible(v) || self.in_image(v) {
                    continue;
                }
                let child = format!("{path}+{o:x}");
                // hkArray<T>: data pointer, i32 size, i32 capacity | flags.
                if o + 16 <= block.len() {
                    let size = u32_at(&block, o + 8) as usize;
                    let cap = (u32_at(&block, o + 12) & 0x3FFF_FFFF) as usize;
                    if (MIN_RECORDS..=MAX_RECORDS * 2).contains(&size) && cap >= size && cap < 0x10000 {
                        self.test_array(&format!("{child}[{size}]"), v, Some(size));
                    }
                }
                if depth + 1 < MAX_DEPTH && !self.visited.contains(&v) {
                    queue.push_back((format!("{child}>"), v, depth + 1));
                }
            }
        }
    }
}

// ---- the task -----------------------------------------------------------------------------------------------------

struct Sampled {
    cand: Candidate,
    prev: Option<Vec<Mat3>>,
    /// Largest per-bone turn since the last log line, degrees, and how many bones turned > 5° in that window.
    max_turn: f32,
    moving: HashSet<usize>,
    failed: u32,
}

pub struct PoseProbe {
    spawned_at: Option<u64>,
    walked: bool,
    sampled: Vec<Sampled>,
    lines: u32,
}

impl PoseProbe {
    pub fn new() -> Self {
        Self { spawned_at: None, walked: false, sampled: Vec::new(), lines: 0 }
    }

    pub fn run(&mut self) {
        let frame = bridge::FRAMES.load(Ordering::Relaxed);
        // SAFETY: task callbacks run on the game's main thread.
        let Some(player) = (unsafe { game::main_player() }) else {
            self.spawned_at = None;
            return;
        };
        let spawned = *self.spawned_at.get_or_insert(frame);
        if !self.walked {
            if frame - spawned >= WALK_AFTER_FRAMES {
                self.walked = true;
                self.walk(player);
            }
            return;
        }
        if self.lines >= MAX_LOG_LINES || self.sampled.is_empty() {
            return;
        }
        if frame % SAMPLE_EVERY == 0 {
            for s in &mut self.sampled {
                sample(s);
            }
        }
        if frame % LOG_EVERY == 0 {
            let snap = game::snapshot(player);
            let mut line = format!("anim={} dodging={}", snap.anim_id, snap.dodging as u8);
            for (i, s) in self.sampled.iter_mut().enumerate() {
                let _ = write!(line, " | c{i} {:.0}° {}/{}", s.max_turn, s.moving.len(), s.cand.count);
                if s.failed > 0 {
                    let _ = write!(line, " fail{}", s.failed);
                }
                s.max_turn = 0.0;
                s.moving.clear();
            }
            crate::info!("pose", "{line}");
            let chr = &player.chr_ins as *const _ as usize;
            if let Some(k) = key_bones(chr) {
                crate::info!("pose", "  {k}");
            }
            self.lines += 1;
            if self.lines == MAX_LOG_LINES {
                crate::info!("pose", "sampling done");
            }
        }
    }

    fn walk(&mut self, player: &eldenring::cs::PlayerIns) {
        let started = std::time::Instant::now();
        let chr = &player.chr_ins as *const _ as usize;
        let ctrl = player.chr_ins.chr_ctrl.as_ptr() as usize;
        let modules = player.chr_ins.modules.as_ptr() as usize;
        let model_item = player.chr_ins.chr_model_ins.model_ins.model_item.as_ptr() as usize;
        // Roots: the pointer *values* stored in the opaque fields (docs/POSE-PLAN.md "Research results").
        let fields: [(&str, usize); 13] = [
            ("chr+398 hka_pose_importer", chr + 0x398),
            ("chr+3a0", chr + 0x3A0),
            ("chr+3a8 anim_skeleton_to_model_modifier", chr + 0x3A8),
            ("chr+3b0", chr + 0x3B0),
            ("chr+3b8", chr + 0x3B8),
            ("model_item+640 mtx43_array_entity", model_item + 0x640),
            ("model_item+650 dmypoly_location_modifier", model_item + 0x650),
            ("model_item+658 aabb_exporter", model_item + 0x658),
            ("ctrl+20 animation_ctrl", ctrl + 0x20),
            ("ctrl+28 ragdoll_ins", ctrl + 0x28),
            ("ctrl+178 walk_twist", ctrl + 0x178),
            ("ctrl+180 joint_modifier", ctrl + 0x180),
            ("modules+f0 bonemove", modules + 0xF0),
        ];
        let mut roots = Vec::new();
        for (name, field) in fields {
            match read_u64(field) {
                Some(v) => roots.push((name.to_string(), v as usize)),
                None => crate::warn!("pose", "root {name}: unreadable field {field:#x}"),
            }
        }
        roots.push(("behavior_module".to_string(), player.chr_ins.modules.behavior.as_ptr() as usize));
        crate::info!(
            "pose",
            "walk: chr={chr:#x} ctrl={ctrl:#x} model_item={model_item:#x} roots: {}",
            roots.iter().map(|(n, v)| format!("{n}={v:#x}")).collect::<Vec<_>>().join(", ")
        );

        let mut w = Walk::new();
        w.run(roots);
        crate::info!(
            "pose",
            "walk done in {} ms: {} objects, {} transform arrays, {} name lists",
            started.elapsed().as_millis(),
            w.visited.len(),
            w.transforms.len(),
            w.names.len()
        );
        for (i, c) in w.transforms.iter().enumerate().take(40) {
            crate::info!("pose", "T{i}: {} x {:?} @ {:#x} {}", c.count, c.kind, c.addr, c.path);
        }
        for n in w.names.iter().take(20) {
            let head: Vec<&str> = n.names.iter().take(12).map(String::as_str).collect();
            crate::info!("pose", "N: {} names {}: {} ...", n.names.len(), n.path, head.join(", "));
        }
        for c in &w.transforms {
            if let Some(n) = w.names.iter().find(|n| n.names.len() == c.count) {
                crate::info!("pose", "count match: {} transforms {} <-> names {}", c.count, c.path, n.path);
            }
        }
        if let Some(dir) = crate::log::LOG_DIR.get() {
            if let Err(e) = std::fs::write(dir.join("pose_probe.txt"), &w.tree) {
                crate::error!("pose", "pose_probe.txt: {e}");
            }
        }

        log_skeleton(chr);

        // Sample the most skeleton-like arrays first: 40..400 records, one per address.
        let mut cands = w.transforms.clone();
        cands.sort_by_key(|c| (!(40..=400).contains(&c.count), c.path.len()));
        let mut seen = HashSet::new();
        self.sampled = cands
            .into_iter()
            .filter(|c| seen.insert(c.addr))
            .take(MAX_SAMPLED)
            .map(|cand| Sampled { cand, prev: None, max_turn: 0.0, moving: HashSet::new(), failed: 0 })
            .collect();
        for (i, s) in self.sampled.iter().enumerate() {
            crate::info!("pose", "sampling c{i} = {} x {:?} {}", s.cand.count, s.cand.kind, s.cand.path);
        }
    }
}

fn sample(s: &mut Sampled) {
    let stride = s.cand.kind.stride();
    let Some(b) = read_bytes(s.cand.addr, s.cand.count * stride) else {
        s.failed += 1;
        return;
    };
    let f = f32s(&b);
    let mut now = Vec::with_capacity(s.cand.count);
    for r in 0..s.cand.count {
        match record_rotation(&f[r * stride / 4..(r + 1) * stride / 4], s.cand.kind) {
            Some(m) => now.push(m),
            None => {
                s.failed += 1;
                return;
            }
        }
    }
    if let Some(prev) = &s.prev {
        for (i, (a, b)) in prev.iter().zip(&now).enumerate() {
            let turn = angle_deg(a, b);
            s.max_turn = s.max_turn.max(turn);
            if turn > 5.0 {
                s.moving.insert(i);
            }
        }
    }
    s.prev = Some(now);
}

// ---- the pose found by the walk (2026-10-04 run 1) -----------------------------------------------------------------
// ChrIns+0x398 → CSFD4LocationHkaPoseImporter; +0x48 hkaSkeleton* (150 bones for c0000), +0x50 hkArray local pose,
// +0x60 hkArray model pose (hkQsTransform). hkaSkeleton +0x30 bones (hkaBone 16 B, name first), +0x40 reference pose.

const IMPORTER: usize = 0x398;
const POSE_SKELETON: usize = 0x48;
const POSE_LOCAL: usize = 0x50;
const POSE_MODEL: usize = 0x60;
/// Bones logged per sample: Pelvis, L_Thigh, Spine, Head, R_UpperArm (indices in the c0000 skeleton).
const KEY_BONES: [(usize, &str); 5] = [(8, "Pelvis"), (10, "L_Thigh"), (47, "Spine"), (86, "Head"), (97, "R_UpperArm")];

/// (data pointer, size) of the hkArray at `addr`.
fn hk_array(addr: usize) -> Option<(usize, usize)> {
    let b = read_bytes(addr, 16)?;
    Some((u64_at(&b, 0) as usize, u32_at(&b, 8) as usize))
}

fn qs(addr: usize, index: usize) -> Option<Vec<f32>> {
    read_bytes(addr + index * 48, 48).map(|b| f32s(&b))
}

/// Once: the hkaPose header, the skeleton header (every hkArray in it) and the parent-index array (i16, parents[i] < i, root -1).
fn log_skeleton(chr: usize) {
    let Some(importer) = read_u64(chr + IMPORTER).map(|v| v as usize) else { return };
    let hex = |addr: usize, len: usize| {
        read_bytes(addr, len).map_or("unreadable".into(), |b| {
            (0..len).step_by(8).map(|o| format!("{:x}", u64_at(&b, o))).collect::<Vec<_>>().join(" ")
        })
    };
    crate::info!("pose", "importer+40..+a0: {}", hex(importer + 0x40, 0x60));
    let Some(skel) = read_u64(importer + POSE_SKELETON).map(|v| v as usize) else { return };
    crate::info!("pose", "skeleton+0..+80: {}", hex(skel, 0x80));
    for o in (0..0x80).step_by(8) {
        let Some((ptr, size)) = hk_array(skel + o) else { continue };
        if !plausible(ptr) || size == 0 || size > 4096 {
            continue;
        }
        let Some(raw) = read_bytes(ptr, size * 2) else { continue };
        let parents: Vec<i16> = raw.chunks_exact(2).map(|c| i16::from_le_bytes([c[0], c[1]])).collect();
        let is_parents = parents[0] == -1 && parents.iter().enumerate().skip(1).all(|(i, &p)| p >= 0 && (p as usize) < i);
        crate::info!("pose", "skeleton+{o:x}: hkArray size={size} {}", if is_parents { "= PARENTS (i16)" } else { "" });
        if is_parents {
            let list: Vec<String> = parents.iter().enumerate().map(|(i, p)| format!("{i}<{p}")).collect();
            crate::info!("pose", "parents: {}", list.join(" "));
        }
    }
    // Bind (reference) pose of the key bones, local space.
    if let Some((refp, n)) = hk_array(skel + 0x40) {
        for (i, name) in KEY_BONES {
            if i < n {
                if let Some(f) = qs(refp, i) {
                    crate::info!("pose", "bind {name}: t=({:.3},{:.3},{:.3}) q=({:.3},{:.3},{:.3},{:.3})", f[0], f[1], f[2], f[4], f[5], f[6], f[7]);
                }
            }
        }
    }
}

/// Local and model transforms of KEY_BONES, re-resolved from ChrIns every call (survives array reallocation).
fn key_bones(chr: usize) -> Option<String> {
    let importer = read_u64(chr + IMPORTER)? as usize;
    let (local, nl) = hk_array(importer + POSE_LOCAL)?;
    let (model, nm) = hk_array(importer + POSE_MODEL)?;
    let mut out = String::new();
    for (i, name) in KEY_BONES {
        if i >= nl || i >= nm {
            continue;
        }
        let (l, m) = (qs(local, i)?, qs(model, i)?);
        let _ = write!(
            out,
            "{name} L=({:.2},{:.2},{:.2},{:.2}) M=({:.2},{:.2},{:.2},{:.2}) Mt=({:.2},{:.2},{:.2}); ",
            l[4], l[5], l[6], l[7], m[4], m[5], m[6], m[7], m[0], m[1], m[2]
        );
    }
    Some(out)
}
