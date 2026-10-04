//! Pose retarget math (docs/POSE-PLAN.md steps 4–5): quaternions, the ER → Skyrim model basis, bind deltas.
//!
//! - Quaternions are `[x, y, z, w]`, Hamilton product, rotating column vectors (`v' = q v q*`), as Havok and NetImmerse do.
//! - A **basis** maps ER model coordinates to Skyrim model coordinates (x right, y forward, z up). ER is Y-up and left-handed
//!   (coords.rs: right × forward = down), so the map can be a reflection. Rotations are converted as matrices, `B·R·Bᵀ`, which is
//!   a proper rotation either way (a reflected quaternion can't be written as `b·q·b*`).
//! - A bone's **delta** is `q_model · q_bind_model⁻¹`: the model-space rotation that takes its bind orientation to the current one.
//!   Skyrim applies `delta · host_bind_model`, so both skeletons only need to agree on where their limbs point at bind.
//!   (Idea from 2010-rust-rewrite-mashup `crates/render_anim/src/skate/rig.rs`, re-implemented here.)

pub type Quat = [f32; 4];
pub type Vec3 = [f32; 3];
pub type Mat3 = [[f32; 3]; 3];

pub const IDENTITY: Quat = [0.0, 0.0, 0.0, 1.0];

pub fn mul(a: Quat, b: Quat) -> Quat {
    let [ax, ay, az, aw] = a;
    let [bx, by, bz, bw] = b;
    [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]
}

pub fn conj(q: Quat) -> Quat {
    [-q[0], -q[1], -q[2], q[3]]
}

pub fn norm(q: Quat) -> f32 {
    (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]).sqrt()
}

/// Unit length, `w ≥ 0`; `None` for a zero or non-finite input.
pub fn normalize(q: Quat) -> Option<Quat> {
    let n = norm(q);
    if !n.is_finite() || n < 1e-6 {
        return None;
    }
    let s = if q[3] < 0.0 { -1.0 / n } else { 1.0 / n };
    Some([q[0] * s, q[1] * s, q[2] * s, q[3] * s])
}

pub fn axis_angle(axis: Vec3, angle: f32) -> Quat {
    let (s, c) = (angle * 0.5).sin_cos();
    [axis[0] * s, axis[1] * s, axis[2] * s, c]
}

/// Rotation angle of `q` in radians (0..π).
pub fn angle(q: Quat) -> f32 {
    2.0 * q[3].abs().min(1.0).acos()
}

pub fn rotate(q: Quat, v: Vec3) -> Vec3 {
    let p = mul(mul(q, [v[0], v[1], v[2], 0.0]), conj(q));
    [p[0], p[1], p[2]]
}

/// Rotation matrix (`m[row][col]`, `v' = m · v`) of a unit quaternion.
pub fn to_mat(q: Quat) -> Mat3 {
    let [x, y, z, w] = q;
    [
        [1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)],
        [2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)],
        [2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)],
    ]
}

/// Unit quaternion (`w ≥ 0`) of a rotation matrix (Shepperd: branch on the largest diagonal term).
pub fn from_mat(m: &Mat3) -> Quat {
    let t = m[0][0] + m[1][1] + m[2][2];
    let q = if t > 0.0 {
        let s = (t + 1.0).sqrt() * 2.0;
        [(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s]
    } else if m[0][0] > m[1][1] && m[0][0] > m[2][2] {
        let s = (1.0 + m[0][0] - m[1][1] - m[2][2]).sqrt() * 2.0;
        [0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s]
    } else if m[1][1] > m[2][2] {
        let s = (1.0 + m[1][1] - m[0][0] - m[2][2]).sqrt() * 2.0;
        [(m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s]
    } else {
        let s = (1.0 + m[2][2] - m[0][0] - m[1][1]).sqrt() * 2.0;
        [(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s]
    };
    normalize(q).unwrap_or(IDENTITY)
}

/// `q_model · q_bind_model⁻¹`, unit, `w ≥ 0`.
pub fn delta(model: Quat, bind: Quat) -> Quat {
    normalize(mul(model, conj(bind))).unwrap_or(IDENTITY)
}

/// Model transform of a child: `(q_parent · q_local, t_parent + q_parent · t_local)` (Havok scale ≈ 1 is ignored).
pub fn compose(parent: (Quat, Vec3), local: (Quat, Vec3)) -> (Quat, Vec3) {
    let r = rotate(parent.0, local.1);
    (mul(parent.0, local.0), [parent.1[0] + r[0], parent.1[1] + r[1], parent.1[2] + r[2]])
}

fn dot(a: Vec3, b: Vec3) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn sub(a: Vec3, b: Vec3) -> Vec3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn scale(a: Vec3, s: f32) -> Vec3 {
    [a[0] * s, a[1] * s, a[2] * s]
}

fn unit(a: Vec3) -> Option<Vec3> {
    let n = dot(a, a).sqrt();
    (n.is_finite() && n > 1e-6).then(|| scale(a, 1.0 / n))
}

/// ER model coordinates → Skyrim model coordinates (x right, y forward, z up). Rows are the Skyrim axes written in ER coordinates.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Basis {
    pub rows: Mat3,
}

impl Basis {
    /// From three directions measured on the ER bind skeleton (e.g. right = R_Thigh − L_Thigh, forward = toes − feet,
    /// up = head − feet). Up is kept, right is made perpendicular to it, forward perpendicular to both (keeping its sign).
    /// `None` if they are degenerate (parallel or zero).
    pub fn from_directions(right: Vec3, forward: Vec3, up: Vec3) -> Option<Self> {
        let up = unit(up)?;
        let right = unit(sub(right, scale(up, dot(right, up))))?;
        let forward = unit(sub(sub(forward, scale(up, dot(forward, up))), scale(right, dot(forward, right))))?;
        Some(Self { rows: [right, forward, up] })
    }

    /// +1 = both spaces have the same handedness, −1 = the map is a reflection (expected for ER → Skyrim).
    pub fn det(&self) -> f32 {
        let [a, b, c] = self.rows;
        a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])
    }

    pub fn vec(&self, v: Vec3) -> Vec3 {
        [dot(self.rows[0], v), dot(self.rows[1], v), dot(self.rows[2], v)]
    }

    /// The same rotation seen in Skyrim's basis: `B · R · Bᵀ`.
    pub fn rot(&self, q: Quat) -> Quat {
        let r = to_mat(q);
        let b = &self.rows;
        let mut br = [[0.0f32; 3]; 3];
        for i in 0..3 {
            for j in 0..3 {
                br[i][j] = (0..3).map(|k| b[i][k] * r[k][j]).sum();
            }
        }
        let mut out = [[0.0f32; 3]; 3];
        for i in 0..3 {
            for j in 0..3 {
                out[i][j] = (0..3).map(|k| br[i][k] * b[j][k]).sum();
            }
        }
        from_mat(&out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::{FRAC_PI_2, PI};

    fn near_v(a: Vec3, b: Vec3) -> bool {
        (0..3).all(|i| (a[i] - b[i]).abs() < 1e-4)
    }

    /// Same rotation (q and −q are equal rotations).
    fn same_rot(a: Quat, b: Quat) -> bool {
        (dot([a[0], a[1], a[2]], [b[0], b[1], b[2]]) + a[3] * b[3]).abs() > 1.0 - 1e-5
    }

    /// ER's measured axes (coords.rs, yaw 0): forward = −Z, right = −X, up = +Y.
    fn er_basis() -> Basis {
        Basis::from_directions([-1.0, 0.0, 0.0], [0.0, 0.0, -1.0], [0.0, 1.0, 0.0]).unwrap()
    }

    #[test]
    fn matrix_round_trip() {
        for q in [IDENTITY, axis_angle([0.0, 0.0, 1.0], PI), axis_angle([0.6, 0.0, 0.8], 2.0), [0.70, 0.70, -0.05, -0.12]] {
            let q = normalize(q).unwrap();
            assert!(same_rot(from_mat(&to_mat(q)), q), "{q:?}");
            assert!(near_v(rotate(q, [1.0, 2.0, 3.0]), {
                let m = to_mat(q);
                [dot(m[0], [1.0, 2.0, 3.0]), dot(m[1], [1.0, 2.0, 3.0]), dot(m[2], [1.0, 2.0, 3.0])]
            }));
        }
    }

    #[test]
    fn identity_bind_gives_identity_delta() {
        let bind = normalize([0.707, 0.707, 0.0, 0.0]).unwrap();
        assert!(same_rot(delta(bind, bind), IDENTITY));
        let turn = axis_angle([0.0, 1.0, 0.0], 0.5);
        assert!(same_rot(delta(mul(turn, bind), bind), turn), "delta recovers the model-space turn");
    }

    #[test]
    fn er_basis_is_a_reflection_and_maps_axes() {
        let b = er_basis();
        assert!((b.det() + 1.0).abs() < 1e-5, "ER is left-handed: det {}", b.det());
        assert!(near_v(b.vec([0.0, 0.0, -1.0]), [0.0, 1.0, 0.0]), "ER forward → Skyrim +Y");
        assert!(near_v(b.vec([-1.0, 0.0, 0.0]), [1.0, 0.0, 0.0]), "ER right → Skyrim +X");
        assert!(near_v(b.vec([0.0, 1.0, 0.0]), [0.0, 0.0, 1.0]), "ER up → Skyrim +Z");
    }

    #[test]
    fn converted_rotation_moves_points_the_same_way() {
        let b = er_basis();
        for q in [axis_angle([0.0, 1.0, 0.0], 0.7), axis_angle([1.0, 0.0, 0.0], -1.2), normalize([0.3, -0.5, 0.2, 0.8]).unwrap()] {
            let p = [0.3, 1.1, -0.4];
            // Rotate in ER then convert == convert then rotate in Skyrim.
            assert!(near_v(b.vec(rotate(q, p)), rotate(b.rot(q), b.vec(p))), "{q:?}");
        }
    }

    #[test]
    fn turning_right_stays_turning_right() {
        // A point ahead of the character moves to its right: the same in both games, whatever the handedness.
        let b = er_basis();
        let er_ahead = [0.0, 0.0, -1.0];
        let er_right = [-1.0, 0.0, 0.0];
        // Find the ER rotation about up that takes ahead → right (sign depends on handedness, so test both).
        let q = [axis_angle([0.0, 1.0, 0.0], FRAC_PI_2), axis_angle([0.0, 1.0, 0.0], -FRAC_PI_2)]
            .into_iter()
            .find(|q| near_v(rotate(*q, er_ahead), er_right))
            .unwrap();
        assert!(near_v(rotate(b.rot(q), [0.0, 1.0, 0.0]), [1.0, 0.0, 0.0]), "Skyrim ahead → right");
    }

    #[test]
    fn basis_from_skewed_directions() {
        // Measured directions are never exact: right with some up in it, forward with some right in it.
        let b = Basis::from_directions([-1.0, 0.1, 0.05], [0.2, -0.1, -1.0], [0.0, 2.0, 0.0]).unwrap();
        assert!(near_v(b.rows[2], [0.0, 1.0, 0.0]));
        assert!((b.det().abs() - 1.0).abs() < 1e-5);
        assert!(Basis::from_directions([1.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]).is_none(), "degenerate");
    }

    #[test]
    fn compose_chains_model_transforms() {
        let parent = (axis_angle([0.0, 0.0, 1.0], FRAC_PI_2), [1.0, 0.0, 0.0]);
        let (q, t) = compose(parent, (IDENTITY, [1.0, 0.0, 0.0]));
        assert!(near_v(t, [1.0, 1.0, 0.0]), "{t:?}");
        assert!(same_rot(q, parent.0));
    }
}
