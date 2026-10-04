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
