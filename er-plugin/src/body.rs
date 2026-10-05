//! `[body]` telemetry (2026-10-05: the run/sprint body direction and lean looked off in Skyrim). Where the body faces and leans
//! relative to where the character travels, in the same terms as Skyrim's `[body]` line (skse/src/bridge/Pose.cpp), so the two sides
//! compare without matching clocks. Degrees; turning right, leaning forward and leaning right are positive. One summary line per
//! SUMMARY_FRAMES frames of movement (mean/largest).
//!
//! ER draws the body with `ChrCtrl.model_matrix` (physics orientation × `additional_orientation_quat`, then eased: eldenring-rs
//! `cs/chr_ins.rs`), while PlayerState.yaw is the physics yaw; `face` vs `model` shows how far apart they are, `tilt` what the extra
//! orientation adds. The bones are in the model's frame, so hip/chest/lean are placed with the model yaw.

use skyrimxer_protocol::proto::PoseBone;
use skyrimxer_protocol::rig::Vec3;

use eldenring::cs::PlayerIns;
use fromsoftware_shared::F32ModelMatrix;

const SUMMARY_FRAMES: u32 = 120;
/// Moving = faster than 0.5 m/s at 60 fps; slower frames have no travel direction.
const MIN_STEP_M: f32 = 0.5 / 60.0;
/// Larger jumps are teleports/reloads (park.rs MAX_STEP_M).
const MAX_STEP_M: f32 = 1.5;

fn wrap(a: f32) -> f32 {
    let t = std::f32::consts::TAU;
    a - t * (a / t).round()
}

#[derive(Default, Clone, Copy)]
struct Stat {
    sum: f32,
    /// The value with the largest magnitude, signed.
    max: f32,
}

impl Stat {
    fn add(&mut self, rad: f32) {
        let v = rad.to_degrees();
        self.sum += v;
        if v.abs() > self.max.abs() {
            self.max = v;
        }
    }

    fn show(&self, n: u32) -> String {
        format!("{:.0}/{:.0}", self.sum / n as f32, self.max)
    }
}

#[derive(Default)]
struct Window {
    n: u32,
    face: Stat,
    model: Stat,
    hip: Stat,
    chest: Stat,
    lean_f: Stat,
    lean_s: Stat,
    tilt_f: Stat,
    tilt_s: Stat,
    /// Physics yaw read back from physics_model_matrix (rows / columns as model axes): picks the matrix layout.
    rows_err: Stat,
    cols_err: Stat,
}

#[derive(Default)]
pub struct BodyProbe {
    last: Option<[f32; 3]>,
    w: Window,
    anim: i32,
}

/// Yaw (player convention: forward = (−sin y, −cos y) in XZ) and up axis of a model matrix whose axes are its rows or its columns.
/// Model forward is −Z (docs/research/elden-ring-pose.md), so forward = −axis2.
fn yaw_up(m: &F32ModelMatrix, rows: bool) -> (f32, Vec3) {
    let r = [[m.0.0, m.0.1, m.0.2], [m.1.0, m.1.1, m.1.2], [m.2.0, m.2.1, m.2.2]];
    let axis = |i: usize| if rows { r[i] } else { [r[0][i], r[1][i], r[2][i]] };
    let z = axis(2);
    (f32::atan2(z[0], z[2]), axis(1))
}

impl BodyProbe {
    /// Every frame: `at` = the mapped bones' model positions in Skyrim's basis (x right, y forward, z up), metres.
    pub fn update(&mut self, player: &PlayerIns, physics_yaw: f32, virtual_pos: Option<[f32; 3]>, anim: i32, at: &[Vec3]) {
        let Some(p) = virtual_pos else {
            self.last = None;
            return;
        };
        let last = self.last.replace(p);
        let Some(last) = last else { return };
        let (dx, dz) = (p[0] - last[0], p[2] - last[2]);
        let step = dx.hypot(dz);
        if !(MIN_STEP_M..=MAX_STEP_M).contains(&step) {
            return;
        }
        let travel = f32::atan2(-dx, -dz);
        let ctrl = &player.chr_ins.chr_ctrl;
        let (phys_rows, _) = yaw_up(&ctrl.physics_model_matrix, true);
        let (phys_cols, _) = yaw_up(&ctrl.physics_model_matrix, false);
        let rows = wrap(phys_rows - physics_yaw).abs() <= wrap(phys_cols - physics_yaw).abs();
        let (model_yaw, up) = yaw_up(&ctrl.model_matrix, rows);

        let w = &mut self.w;
        w.n += 1;
        self.anim = anim;
        w.rows_err.add(wrap(phys_rows - physics_yaw));
        w.cols_err.add(wrap(phys_cols - physics_yaw));
        w.face.add(wrap(physics_yaw - travel));
        let d = wrap(model_yaw - travel);
        w.model.add(d);
        // Tilt of the model's up axis, in the model's facing frame (ER world: forward (−sin y, 0, −cos y), right (−cos y, 0, sin y)).
        let (s, c) = model_yaw.sin_cos();
        w.tilt_f.add(f32::atan2(-s * up[0] - c * up[2], up[1]));
        w.tilt_s.add(f32::atan2(-c * up[0] + s * up[2], up[1]));
        // Body axes in the model frame (Skyrim basis), placed with the model yaw. Axis angle: turning right positive.
        let axis = |l: PoseBone, r: PoseBone| {
            let (a, b) = (at[l as usize], at[r as usize]);
            wrap(d + f32::atan2(-(b[1] - a[1]), b[0] - a[0]))
        };
        w.hip.add(axis(PoseBone::LThigh, PoseBone::RThigh));
        w.chest.add(axis(PoseBone::LUpperArm, PoseBone::RUpperArm));
        // Trunk (pelvis → neck) in the travel frame: body right = (cos d, −sin d), body forward = (sin d, cos d).
        let (pel, neck) = (at[PoseBone::Pelvis as usize], at[PoseBone::Neck as usize]);
        let v = [neck[0] - pel[0], neck[1] - pel[1], neck[2] - pel[2]];
        let (sd, cd) = d.sin_cos();
        w.lean_f.add(f32::atan2(-v[0] * sd + v[1] * cd, v[2]));
        w.lean_s.add(f32::atan2(v[0] * cd + v[1] * sd, v[2]));

        if w.n >= SUMMARY_FRAMES {
            let n = w.n;
            crate::info!(
                "body",
                "n={n} anim={} deg mean/max vs travel: face={} model={} hip={} chest={} leanF={} leanS={} | tiltF={} tiltS={} | \
                 phys matrix yaw err rows={} cols={}",
                self.anim,
                w.face.show(n),
                w.model.show(n),
                w.hip.show(n),
                w.chest.show(n),
                w.lean_f.show(n),
                w.lean_s.show(n),
                w.tilt_f.show(n),
                w.tilt_s.show(n),
                w.rows_err.show(n),
                w.cols_err.show(n)
            );
            self.w = Window::default();
        }
    }
}
