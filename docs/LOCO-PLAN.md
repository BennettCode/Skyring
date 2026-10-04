# LOCO-PLAN: Elden Ring drives the Skyrim body (smooth rolls, then ER locomotion)

Approved 2026-10-04. Status: Stage A mostly done (protocol v5, timeline with 0 late frames, pose fixes, whole-roll follow). **Stuck: smooth player movement** (`ApplyCurrent` refuses currents, ~23 of 60 roll frames don't move): see STATUS handoff. Stage B not started.

**Correction after approval (measured):** the player's third-person skeleton has 51 nodes and no finger or toe bones (the hand/foot meshes reference them, but they resolve to nothing), so PoseBone grows 20 → 24 (the four upper-arm twist bones), not 56.

## Context
The user played the streamed roll (2026-10-04) and found four problems:
1. **Jittery.** Measured cause: `movement` applies the whole gap to ER's position in one frame and `ApplyCurrent` skips frames
   (`applied=false`, speeds 93 → 355 → −90 u/s). Also the two games' frames aren't phase-locked: Skyrim reads some ER frames twice and
   skips others (`er_frame 22771 → 22773`), so the pose stutters.
2. **Distance feels off.** Skyrim matches ER only until the move-cancel window (99% of 3.2 m), then hands back. The bursts make it read wrong.
3. **Arms stretch.** Skyrim's upper arms are skinned mostly to `NPC L/R UpperarmTwist1/2`, children we don't pose. They keep Skyrim's
   animated twist while we rotate the upper arm. The shape-bone fits (clavicle 37°, Spine2 14°) also move the shoulder joints.
4. **Running after a roll is weird.** Movement hands back at move-cancel (~frame 43), but the pose plays ER's recovery until ~60 frames
   while Skyrim already runs. The blend then goes into Skyrim's run, not ER's.

**Decision (user, 2026-10-04): full ER locomotion,** the way both skate mashups do it. GTA San AnSkateas and the MW2 + Skate 3 mashup
toggle a mode (J): inside it the hidden engine owns the whole body's pose, movement and controls on the host's collision; outside it the
host walks. Here the mode is the bridge (F10). While it's on and both games are in-world, ER drives walking, running, sprinting and
rolling, with ER's animations and speeds. Skyrim keeps the world, collision, NPCs, camera and HP/death rules.

## Authority change (DESIGN §3, proven by tests in stage B)
While the bridge is on, ER owns locomotion (speed, facing, animation) and Skyrim's position follows ER's virtual displacement through
Skyrim's character controller (Skyrim collision still stops it). Skyrim's own walk/run/sprint input goes to ER, not to Skyrim's movement.
Off / menus / ER stale → vanilla Skyrim within 250 ms (fail-safe unchanged).

## Stage A: smooth and faithful (protocol v5; makes rolls right before locomotion builds on it)
1. **Protocol v5** (`protocol/schema/messages.toml`, plan-mode item):
   - `time_us` (QueryPerformanceCounter µs, the same clock in both processes) in PlayerState and PoseState.
   - `PoseBone` grows from 20 to 56 bones:
     - + L/R upper-arm twist ×2 (ER `L_UpArmTwist`, `L_UpArmTwist1` → Skyrim `UpperarmTwist1/2`);
     - + toes (ER `L_Toe0` → `NPC L Toe0`);
     - + 15 fingers per hand (ER `L_Finger{0-4}{"",1,2}` → Skyrim `NPC L Finger{0-4}{0-2}`).
   - New slot `PoseBind` at 0x800: seq, bone_count, ER bind segment direction per bone (Skyrim basis), written once per ER skeleton.
     Skyrim computes the fits from it, so the hardcoded table in Pose.cpp goes.
   - Run protogen, update fake-peer and the slot tests; run-tests green.
2. **ER writer** (`er-plugin/src/pose_stream.rs`): the new bones, `time_us`, and the PoseBind write when the skeleton resolves.
   `remote::publish_state` gets `time_us` too.
3. **Skyrim ER timeline** (new `skse/src/bridge/Timeline.{h,cpp}`):
   - A history of PlayerState + PoseState samples on `time_us`. Render slightly in the past with an adaptive delay that follows how
     late samples arrive. Interpolate, never extrapolate.
   - Copied and adapted from SkyCraft `skse/src/Game.cpp:662-790` (MIT). Same commit: NOTICES row, `licenses/SkyCraft.txt`,
     source comment, README credit.
   - Gives interpolated pose (per-bone slerp, pelvis lerp), position and yaw for any Skyrim frame.
4. **Movement from the timeline** (`skse/src/bridge/Movement.cpp`):
   - Velocity = feed-forward (interpolated ER displacement this frame / dt) + a gentle correction (~8/s gain, capped), instead of
     "close the whole gap in one frame".
   - Log `ApplyCurrent` misses and per-frame speed. Follow ER through the whole dodge animation, not just to move-cancel.
5. **Pose applier fixes** (`skse/src/bridge/Pose.cpp`):
   - Fits only on limbs, fingers and toes (shape bones pelvis/spine/neck/head/clavicle use identity).
   - Twist bones and fingers are posed from ER.
   - Unmapped skinned helpers (pauldrons, etc.) keep their bind offset to their posed parent while posing (mashup `rig.rs` rule:
     unmapped bones inherit).
   - Extend the `[pose] check` line to all mapped bones.
- **Accept A** (agent-run: auto load-in + key scripts + `tools/screenshot.ps1`):
  - **Smooth:** no frame with zero movement mid-roll; frame-to-frame speed change under 15% of peak (new `[move]` summary line).
  - **Distance:** Skyrim distance over the whole roll ≥ 97% of ER's.
  - **Pose:** check-line error ≤ 3° on all mapped bones. No visible stretch on contact sheets, side by side with an ER-visible
    reference capture of the same roll (`-ErVisible`, same key script on ER).
  - Then the user judges.

## Stage B: ER locomotion (the mode)
1. **Research probe (ER only):**
   - Read ER camera yaw (eldenring-rs `cs/camera.rs` `CSCamera`/`CSCam` view matrix).
   - Confirm a held virtual stick walks/runs the pinned character (park.rs virtual position) and that a held Dodge = ER sprint.
   - Notes go in `docs/research/elden-ring-input.md`.
2. **Input** (`er-plugin/src/remote.rs`):
   - In the mode, Skyrim's move keys go to ER's stick every frame, not just around a dodge.
   - A held Left Shift goes to ER as Dodge (ER: tap = roll, hold = sprint); vanilla Skyrim sprint stays off.
   - Skyrim's own movement input stays swallowed (MovementHandler hook exists).
3. **Mapping:**
   - `off = ER camera yaw − Skyrim camera yaw`, sent each frame in PlayerState (new field `cam_yaw`, protocol v5 from stage A).
   - ER then turns its character toward the stick relative to its own camera, i.e. Skyrim's look + stick, offset by `off`.
   - Skyrim body facing = ER yaw − off. Displacement goes ER local → Skyrim world with that heading (existing `coords` functions).
   - Movement and pose stay always on in the mode, through the Stage A timeline. Rolls are no longer a special case: they're just ER motion.
4. **Skyrim side:** Pose Active whenever ER is in the world and the mode is on (the ER writer sets Active always in the mode).
   Skyrim's own animation still runs underneath (other systems need it) but is fully overridden. First person: movement from ER, no pose.
5. **Out of scope for B (noted in docs):** jump, swimming, mounts and ladders fall back to vanilla. Leaving the mode for them is automatic:
   swimming/mounted/in a menu → mode suspended.
- **Accept B** (agent-run, then the user):
  - W/A/S/D walk and run with ER's animation and speed, on Skyrim's collision.
  - A roll flows into ER's run with no hand-back.
  - Sprint (hold Left Shift) is ER's sprint and costs ER stamina.
  - Turning is camera-relative like ER.
  - Frame time unchanged.
  - F10 off → vanilla walking at once.

## Files
- **Protocol:** `protocol/schema/messages.toml` + `generated/*`, `tools/fake-peer`, `protocol/src/slot.rs` tests,
  `skse/tests/link_test.cpp`, `tests/run-tests.ps1`.
- **ER:** `er-plugin/src/{pose_stream.rs, remote.rs, park.rs, game.rs}`.
- **Skyrim:** `skse/src/bridge/{Timeline.* (new), Movement.*, Pose.cpp, Bridge.cpp}`.
- **Docs:** `docs/LOCO-PLAN.md` (this plan, public copy), DESIGN §3/§4, README, THIRD-PARTY-NOTICES, MODLOG, STATUS.

## Reuse
- SkyCraft interpolation (`reference/SkyCraft/skse/src/Game.cpp:662-790`).
- Mashup unmapped-bone rule (`reference/2010-rust-rewrite-mashup/crates/render_anim/src/skate/rig.rs`).
- Ours:
  - `park.rs` (pin + virtual position), `coords` (ER ↔ Skyrim deltas), `rig.rs`/`Rig.h` (math);
  - `tools/dev.ps1` auto load-in, `tools/game-input.ps1` (keys + mouse), `tools/screenshot.ps1` (contact sheets).

## Verification
- `tests/run-tests.ps1` after the protocol change (v5 slots + PoseBind round trips, interop pose checks).
- Every stage runs agent-side first: `tools/dev.ps1 -Restart`, then key scripts (roll each direction, run, roll → run, sprint).
  I read the `[move]`/`[pose]` summary lines and contact sheets next to an ER-visible reference capture.
- Then the user plays and judges.
- Commit after each accepted stage. Stuck rule: 2 failed attempts at one problem → handoff.
