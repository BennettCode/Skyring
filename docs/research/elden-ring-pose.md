# Elden Ring: the player's skeleton pose (POSE-PLAN step 1)

Found 2026-10-04 with `er-plugin/src/pose.rs` (`tools/dev.ps1 -ErPoseProbe`). The probe walks memory from the opaque pointers on ChrIns, using safe
reads only (ReadProcessMemory on the own process), names each object by its MSVC RTTI, and tests arrays for transforms and bone names.
Game 2.7.1.0 (1.17.1 WW), eldenring-rs 59fbd3b, character c0000 (the player). Two runs, same results.

## Where it lives
| Address | What (RTTI / evidence) |
|---|---|
| `ChrIns + 0x398` (eldenring-rs `hka_pose_importer`, private) | → `CSFD4LocationHkaPoseImporter` |
| importer `+0x48` | `hkaSkeleton*` (RTTI `hkaSkeleton`) |
| importer `+0x50` | hkArray<hkQsTransform> **local pose** (size 150, capacity 152) |
| importer `+0x60` | hkArray<hkQsTransform> **model pose** (size 150) |
| importer `+0x70` | another hkArray of size 150 (bone flags, presumably). `+0x80` = 1, `+0x90` = 0x8000000000000000 |
| skeleton `+0x20` | hkArray<i16> **parent indices** (root −1, parent < child) |
| skeleton `+0x30` | hkArray<hkaBone> (16 B: name pointer first) → bone names |
| skeleton `+0x40` | hkArray<hkQsTransform> **reference (bind) pose**, local space |

The layout matches Havok's `hkaPose` (skeleton, localPose, modelPose, boneFlags), embedded at importer +0x48.
- **hkQsTransform** (48 B): translation `f32[4]`, rotation quaternion `(x, y, z, w)`, scale `f32[4]` (≈ 1).
- **Units:** metres. Model space is **Y-up**, relative to the character root (`Master`, at the feet). The character's world position and facing are not in the pose; they are in `ChrCtrl` `model_matrix` / the physics module.
- **Reading:** done in `ChrIns_PostPhysics`. The model pose was up to date there on every sample: all 150 bones changed during rolls.
- Other arrays found and ruled out:
  - 81-bone poses under `ChrIns+0x3B0`: they move while the player is idle, so they belong to the enemy parked nearby.
  - Ragdoll (18 bones).
  - DummyPoly / AEG (33 entries).
  - Matrix arrays under the model item: 87 × 3x4, likely skinning.

## Measured
- **Idle:** every bone turns less than about 1° per 4 frames. Pelvis model height is 0.94 m, head 1.53 m.
- **Roll (anim 27110):**
  - Local pose: up to 159° per 4 frames, about 70 of 150 bones moving.
  - Model pose: all 150 bones moving.
  - Mid-roll the pelvis drops to 0.27 m and the head to 0.83 m, and the pelvis quaternion tumbles, e.g. (0.70, 0.70, −0.05, −0.12) → (−0.61, −0.65, −0.14, 0.42).
- **After the roll:** the model pose returns to idle (facing lives outside the pose).

**Bind (local) samples:**
| Bone | Translation | Quaternion |
|---|---|---|
| Pelvis | (0, 0, 0) | (0.707, 0.707, 0, 0) |
| L_Thigh | (0, 0.103, 0) | (−0.010, 0, −1, 0) |
| Spine | (0, 0.093, 0) | (0.5, 0.5, 0.5, 0.5) |
| Head | (0.105, 0, 0) | (0, 0.088, 0, 0.996) |
| R_UpperArm | (0.150, 0, 0) | (−0.011, −0.008, 0.383, 0.924) |

Bone axes differ per bone (Havok/Max style, X along the bone), so retarget in **model space** with bind deltas (`docs/POSE-PLAN.md`).

## c0000 skeleton (150 bones): the ones the retarget needs
Index (parent):
| Region | Bones |
|---|---|
| Root | Master 0 (−1), RootPos 7 (0) |
| Legs | Pelvis 8 (7); L_Thigh 10 (8), L_Calf 11 (10), L_Foot 15 (11), L_Toe0 19 (15); R_Thigh 30 (8), R_Calf 31 (30), R_Foot 35 (31), R_Toe0 39 (35) |
| Spine | RootRotY 45 (7), RootRotXZ 46 (45), Spine 47 (46), Spine1 48 (47), Spine2 50 (48); Neck 85 (50), Head 86 (85) |
| Left arm | L_Clavicle 53 (50), L_UpperArm 56 (53), L_Forearm 58 (56), L_Hand 61 (58) |
| Right arm | R_Clavicle 94 (50), R_UpperArm 97 (94), R_Forearm 99 (97), R_Hand 102 (99) |
| Weapons | L_Weapon 78 (61), R_Weapon 119 (102) |

The spine hangs off RootPos through RootRotY/RootRotXZ, so it is not a child of the pelvis. The rest are fingers, twist, skirt, armour and mantle helpers, foot IK targets and 20 `Xtra_Multipurpose` bones. The full name list and tree are written to `build/er-plugin/logs/pose_probe.txt` by the probe.

## Streaming it (POSE-PLAN step 4, 2026-10-04)
`er-plugin/src/pose_stream.rs` writes PoseState every frame in `ChrIns_PostPhysics`. It builds the bind pose in model space from the
skeleton's reference pose and parent array, then sends `q_model · q_bind⁻¹` per mapped bone. Measured on the c0000 bind:
- **Model axes:** right = −X (R_Thigh − L_Thigh = (−0.207, 0, 0)), forward = −Z (toes ahead of the feet), up = +Y. Left-handed
  (basis det −1), the same as the world axes in `docs/research/coordinates.md`. Head − feet leans 2.2° back, so up is fixed to +Y.
- **Bind is an A-pose:** upper arms point 45° down and out (Skyrim basis: L_UpperArm (−0.707, −0.016, −0.707)). Skyrim's own bind will
  differ, so step 5 aligns the segments. Unit segment directions in Skyrim's basis (x right, y forward, z up):
  Pelvis/Spine/Spine1 (0, 0, 1), Spine2 (0, −0.116, 0.993), Neck (0, 0.175, 0.985), Clavicles (±1, 0, 0),
  UpperArms (±0.707, −0.016, −0.707), Forearms (±0.705, 0.097, −0.703), Hands (±0.735, 0.107, −0.670),
  Thighs (0, 0.020, −1.000), Calves (0, −0.169, −0.986), Feet (0, 0.774, −0.634). Bind pelvis is 0.86 m above the feet, head 1.45 m.
- **Rolls** (`-ErSelfTest roll`): every direction plays anim **27110**; ER first turns the body toward the roll, so the direction
  travels in `yaw`, not in the pose. Per roll: dodge anim for 101 frames (≈ 1.7 s, recovery included), pelvis turn up to 179°
  (it tumbles), pelvis drop up to 0.84 m. Backsteps (27010): 81 frames, 56°, 0.54 m.
- **Cost:** the write takes ≤ 0.27 ms (max per roll, three ReadProcessMemory calls + the snapshot).

## Rendered facing vs physics yaw (2026-10-05)
ER draws the body with `ChrCtrl.model_matrix` (physics orientation × `additional_orientation_quat`, then eased; eldenring-rs `cs/chr_ins.rs`).
Measured with the `[body]` line (`er-plugin/src/body.rs`) during run, sprint, strafe and turns:
- Its yaw equals the physics yaw (PlayerState.yaw) within 1°.
- Its tilt is at most 4° (forward and sideways).

So the physics yaw is the right facing. The matrix stores the model axes in its **rows** (the yaw read from `physics_model_matrix` rows
matches the physics yaw; the columns don't). ER's own lean, from pelvis to neck: walk 7°, run 16–18°, sprint 35° forward.

## Weapon bones and bows (2026-10-06)
- `R_Weapon` / `L_Weapon` are separate bones near the hands (~9 cm out). In the hand's own frame they stay **constant** through idle,
  walk and most of a swing (R: hand-local x (0.92,-0.29,0.25) y (0.28,0.96,0.07)); some attack frames turn them (23030000).
- The blade runs along the weapon bone's **+Y** (screenshots: longsword idle points forward, R_Weapon +Y = (0.32,0.93,-0.17) Skyrim basis).
- Bows: carried in the **right** hand, sideways (idle 14000000: R +Y = (-0.83,-0.55,0.07)); moved into the left hand only for x36010 draw,
  x36020 hold, x36000 release (L +Y = (0,0,-1) at full draw, so +Y is the bow's lower limb). Contact sheet of a full shot.
- Thumb base `L/R_Finger0` bind direction from the hand (Skyrim basis): R (0.410,0.736,-0.539).
