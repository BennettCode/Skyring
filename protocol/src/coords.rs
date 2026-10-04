//! Coordinate conversion between the two games, measured in P3 (docs/research/coordinates.md, DESIGN §6).
//!
//! The worlds are unrelated, so only **deltas in the character's own frame** cross over: a world delta becomes
//! [`Local`] (forward, right, up in metres) with that game's facing angle, and back into the other game's world with its facing angle.
//! `skse/src/bridge/Coords.h` mirrors this file (same names, same tests).
//!
//! Measured conventions (both games, 2026-10-04):
//! - Skyrim: Z up, 70 units ≈ 1 m. Heading = `data.angle.z`; forward = (sin h, cos h, 0), right = (cos h, −sin h, 0).
//! - Elden Ring: Y up, 1 unit = 1 m. Yaw = rotation about Y; forward = (−sin y, 0, −cos y), right = (−cos y, 0, sin y).
//! - In both, a growing angle = turning right (clockwise seen from above), so yaw deltas carry over with the same sign.

/// Skyrim units per metre (1 unit = 1.428 cm; documented, not measured).
pub const SKYRIM_UNITS_PER_M: f32 = 70.0;

/// A delta in the character's own frame, metres.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Local {
    pub forward: f32,
    pub right: f32,
    pub up: f32,
}

/// ER world delta `[x, y, z]` (metres, Y up) → character frame, for a character facing `yaw`.
pub fn er_delta_to_local(d: [f32; 3], yaw: f32) -> Local {
    let (s, c) = yaw.sin_cos();
    Local { forward: -d[0] * s - d[2] * c, right: -d[0] * c + d[2] * s, up: d[1] }
}

/// Character frame → ER world delta `[x, y, z]` (metres, Y up), for a character facing `yaw`.
pub fn local_to_er_delta(l: Local, yaw: f32) -> [f32; 3] {
    let (s, c) = yaw.sin_cos();
    [-l.forward * s - l.right * c, l.up, -l.forward * c + l.right * s]
}

/// Skyrim world delta `[x, y, z]` (game units, Z up) → character frame (metres), for a character with `heading` (`GetAngleZ()`).
pub fn skyrim_delta_to_local(d: [f32; 3], heading: f32) -> Local {
    let (s, c) = heading.sin_cos();
    Local {
        forward: (d[0] * s + d[1] * c) / SKYRIM_UNITS_PER_M,
        right: (d[0] * c - d[1] * s) / SKYRIM_UNITS_PER_M,
        up: d[2] / SKYRIM_UNITS_PER_M,
    }
}

/// Character frame (metres) → Skyrim world delta `[x, y, z]` (game units, Z up), for a character with `heading`.
pub fn local_to_skyrim_delta(l: Local, heading: f32) -> [f32; 3] {
    let (s, c) = heading.sin_cos();
    [
        (l.forward * s + l.right * c) * SKYRIM_UNITS_PER_M,
        (l.forward * c - l.right * s) * SKYRIM_UNITS_PER_M,
        l.up * SKYRIM_UNITS_PER_M,
    ]
}

/// An ER yaw change (radians) → the Skyrim heading change that turns the same way (both grow when turning right).
pub fn er_yaw_delta_to_skyrim(d: f32) -> f32 {
    d
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::FRAC_PI_2;

    fn near(a: f32, b: f32, tol: f32) -> bool {
        (a - b).abs() <= tol
    }

    #[test]
    fn round_trips() {
        let l = Local { forward: 1.5, right: -0.7, up: 0.3 };
        for angle in [-3.0f32, -1.2, 0.0, 0.4, 1.7382, 3.1] {
            let e = er_delta_to_local(local_to_er_delta(l, angle), angle);
            let s = skyrim_delta_to_local(local_to_skyrim_delta(l, angle), angle);
            for got in [e, s] {
                assert!(near(got.forward, l.forward, 1e-5) && near(got.right, l.right, 1e-5) && near(got.up, l.up, 1e-5), "{got:?}");
            }
        }
    }

    #[test]
    fn cardinal_directions() {
        // Skyrim heading 0 faces +Y; heading +90° faces +X (turned right).
        let fwd = local_to_skyrim_delta(Local { forward: 1.0, ..Default::default() }, 0.0);
        assert!(near(fwd[0], 0.0, 1e-4) && near(fwd[1], 70.0, 1e-4));
        let fwd = local_to_skyrim_delta(Local { forward: 1.0, ..Default::default() }, FRAC_PI_2);
        assert!(near(fwd[0], 70.0, 1e-4) && near(fwd[1], 0.0, 1e-3));
        // ER yaw 0 faces −Z; yaw +90° faces −X; up is +Y.
        let fwd = local_to_er_delta(Local { forward: 1.0, up: 2.0, ..Default::default() }, 0.0);
        assert!(near(fwd[0], 0.0, 1e-6) && near(fwd[1], 2.0, 1e-6) && near(fwd[2], -1.0, 1e-6));
        let fwd = local_to_er_delta(Local { forward: 1.0, ..Default::default() }, FRAC_PI_2);
        assert!(near(fwd[0], -1.0, 1e-6) && near(fwd[2], 0.0, 1e-6));
        // Right = forward at angle + 90° in both games.
        for angle in [0.3f32, -2.0] {
            let r = local_to_er_delta(Local { right: 1.0, ..Default::default() }, angle);
            let f = local_to_er_delta(Local { forward: 1.0, ..Default::default() }, angle + FRAC_PI_2);
            assert!(near(r[0], f[0], 1e-5) && near(r[2], f[2], 1e-5));
            let r = local_to_skyrim_delta(Local { right: 1.0, ..Default::default() }, angle);
            let f = local_to_skyrim_delta(Local { forward: 1.0, ..Default::default() }, angle + FRAC_PI_2);
            assert!(near(r[0], f[0], 1e-3) && near(r[1], f[1], 1e-3));
        }
    }

    /// Real segments from the 2026-10-04 both-games walk (docs/research/coordinates.md).
    #[test]
    fn measured_segments() {
        // Skyrim, W held (frames 372→510, heading 1.6274): 11.5 m forward, small drift.
        let w = skyrim_delta_to_local([173884.2 - 173080.8, -91225.6 - -91143.9, 11095.0 - 11110.9], 1.6274);
        assert!(near(w.forward, 11.52, 0.05) && w.right.abs() < 0.06 * w.forward, "{w:?}");
        // Skyrim, D held (frames 660→792, same heading, strafe without turning): 9.1 m right.
        let d = skyrim_delta_to_local([173964.4 - 174001.6, -91867.7 - -91232.6, 11154.1 - 11083.6], 1.6274);
        assert!(near(d.right, 9.09, 0.05) && d.forward.abs() < 0.05, "{d:?}");
        // ER, W held (frames 3168→3186, yaw 1.7382): straight ahead.
        let w = er_delta_to_local([4.347 - 5.510, 6.756 - 6.634, 5.472 - 5.275], 1.7382);
        assert!(near(w.forward, 1.18, 0.01) && w.right.abs() < 0.01, "{w:?}");
        // ER, first 12 frames after switching to D (3468→3480), seen from the W facing: to the right.
        let d = er_delta_to_local([-4.100 - -4.284, 8.810 - 8.751, 8.226 - 7.462], 1.7382);
        assert!(near(d.right, 0.78, 0.01) && d.forward.abs() < 0.1, "{d:?}");
        // Same sense of turning: W→D turned ER's yaw by +90°.
        assert!(er_yaw_delta_to_skyrim(-2.9742 + std::f32::consts::TAU - 1.7382) > 0.0);
    }
}
