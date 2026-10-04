# Coordinates: Skyrim ⇄ Elden Ring (P3, measured 2026-10-04)

- Game/version: SkyrimSE 1.7.104 | eldenring.exe 2.7.1.0
- Location: Skyrim `TESObjectREFR::data.location` / `data.angle.z` (`GetPosition()` / `GetAngleZ()`, plain reads, no Address Library ID).
  ER `modules.physics.position` and the yaw from `modules.physics.orientation` (`to_euler_angles().1`, eldenring-rs).
- Confidence: verified by test (one both-games walk, 4 segments per game; MODLOG 2026-10-04). Code: `protocol/src/coords.rs`, `skse/src/bridge/Coords.h`.

## Method
Since P3 step 5 the same Skyrim W/A/S/D drives both characters. Both plugins log a bounded `[coords]` sample every 6 frames while moving
(position, facing angle, move input). The user walked W, D, S, A for ~3 s each on flat ground without touching the mouse. Each game is
fitted on its own; the two worlds are unrelated, so only deltas in the character's own frame cross over.

## Results
| | Skyrim | Elden Ring |
|---|---|---|
| Up axis | +Z | +Y (documented; the walk logged small slopes only) |
| Scale | 70 units ≈ 1 m (documented) | 1 unit = 1 m |
| Forward at angle a | (sin a, cos a, 0): a = 0 faces +Y | (−sin a, 0, −cos a): a = 0 faces −Z |
| Right at angle a | (cos a, −sin a, 0) | (−cos a, 0, sin a) |
| Growing angle | turns right (clockwise from above) | turns right (clockwise from above) |

- **Skyrim:** W moved along the heading (2.7° off), D strafed exactly 90.1° right without turning, S 176.5°. A was 76° (an obstacle/slope:
  it also climbed 25 units). Third person: the character turns to the camera's heading on the first moving frame.
- **Elden Ring:** without lock-on the character turns to face where it runs within 3 frames, so the run direction is its facing. All 63
  samples faster than 2 m/s ran at exactly yaw + 180° (measured from +Z towards +X), mean error 1.7°. Runs are 3.93 m/s.
- **ER handedness:** W → D turned the yaw by exactly +90°. ER's camera auto-follows while running sideways, so later segments drift;
  predicting each new facing from the previous segment's camera with "right = yaw + 90°" matched to 0.1° (S: 8.6°, A: 116.4°).
- Holding D/A in ER while hidden makes the character circle (camera follow) and it got stuck on a wall once (speed 0). Expect this when
  Skyrim movement is mirrored without lock-on.

## Fixtures (used in both unit tests)
- Skyrim W, frames 372→510, heading 1.6274: forward 11.52 m, right 0.52 m. Skyrim D, frames 660→792: right 9.09 m, forward −0.02 m.
- ER W, frames 3168→3186, yaw 1.7382: forward 1.18 m, right 0.00 m. ER first 12 frames of D (3468→3480) in the W frame: right 0.78 m.
