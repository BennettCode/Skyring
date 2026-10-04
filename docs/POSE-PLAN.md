# Pose plan: P4 step 5, roll animation by ER pose streaming

## Context
Three attempts at a vanilla Skyrim roll failed: the third-person camera snaps the body back to the camera's direction, so rolls only play forward. The user chose the approach GTA San AnSkateas uses. ER plays the real roll on the hidden character. The ER plugin reads that skeleton's pose every frame and sends it over shared memory. Skyrim writes it onto the player's skeleton.
- No animation files are converted or shipped.
- Rolls face the right direction, because the yaw is applied by us.
- Every later ER action (P5 attacks, weapon arts) gets animated for free.

Status: direction approved (user, 2026-10-04). **Step 1 done** (pose found, `docs/research/elden-ring-pose.md`). **Step 2 done** (write after `PlayerCharacter::Update` shows on screen; `docs/research/skyrim-hooks.md` "Posing the player's skeleton"). **Step 3 done** (protocol v4 `PoseState` at 0x400, fake ER swings a pose, tests green); **Step 4 done** (ER writer `er-plugin/src/pose_stream.rs` + math `protocol/src/rig.rs`; basis and A-pose bind measured, `docs/research/elden-ring-pose.md` "Streaming it"); **Step 5 done** (Skyrim applier `skse/src/bridge/Pose.cpp`; rolls play in Skyrim, 2026-10-04); next: step 6 (docs) and the polish list in `docs/research/skyrim-hooks.md` "Applying the ER pose". `docs/P4-PLAN.md` step 5 links here.

Research results (this session):
- **ER:** eldenring-rs 59fbd3b and libER have no typed pose. The candidates below are all opaque pointers.
  - `ChrIns+0x398 hka_pose_importer` and `+0x3A8 anim_skeleton_to_model_modifier` (`chr_ins.rs:297-299`).
  - `chr_model_ins(+0x50) → model_item(+0x10) → +0x640 mtx43_array_entity`, likely the skinning palette (`chr_ins.rs:825-847`).
  - `ChrCtrl+0x230 model_matrix` is typed.
  - Havok layouts (unverified): `hkaPose` = skeleton*, localPose hkArray<hkQsTransform 48 B>, modelPose. `hkaSkeleton` has bones (name ptrs) at +0x28 and a reference pose at +0x38.
  - The existing dump probe in `er-plugin/src/combat.rs:176-261` can be reused.
- **GTA San AnSkateas** (`reference/GTA-San-AnSkateas/skate-ffi/src/lib.rs:698-1074`, `sa-plugin/src/main.cpp:821-959`):
  - It uses model-space rest-pose deltas: `posed · srcBind⁻¹` applied to the host's bind, plus a facing quaternion. Bind poses come from skin inverse-bind matrices.
  - It maps 20 bones; neck, head and fingers ride on their parents. It keeps the host's bone lengths and writes after all host animation.
- **Skyrim:**
  - Writes go through `NiAVObject::local.rotate` + `UpdateDownwardPass` (both vfuncs, no new Address Library IDs).
  - Hook candidates: our `PlayerCharacter` 0xAD post-call (`skse/src/hooks/PlayerUpdate.cpp`), or vfunc 0x7D `UpdateAnimation`. Whether either runs after the animation graph is **unverified**.
  - Bone names come from `F/FixedStrings.h`; SkyCraft confirms "NPC L Thigh [LThg]" etc.

## Design (authority unchanged: ER decides the motion, Skyrim owns position/collision)
- **Bone set (~20):** pelvis, spine 0/1/2, neck, head, L/R clavicle/upperarm/forearm/hand, L/R thigh/calf/foot. Unmapped bones ride on their parents.
- **ER side:** for each mapped bone, Δ = q_model · q_bind_model⁻¹ in ER model space. This is converted to Skyrim's basis (Y-up → Z-up; facing measured like `coords.rs`), plus the pelvis offset from bind (metres). It is sent with an `Active` flag while a non-idle ER action plays (dodge first).
- **Skyrim side:** q_host_model = yawFix · Δ · hostBind_model.
  - The host bind comes from the body's `NiSkinData` inverse-bind matrices (to verify).
  - `yawFix` = ER body yaw in Skyrim's world minus the Skyrim body heading. This fixes the "forward only" problem.
  - Model rotations become locals through the parents' model rotations. Then one `UpdateDownwardPass` from the pelvis.
  - Blend in and out over about 5 frames on `Active`, so Skyrim's own locomotion plays the rest of the time.
- **Protocol v4:** a new seqlock slot `PoseState` at 0x400 (space 0x400–0xFFF is free).
  - Fields: seq, flags (Active), frame, time_ms, bone_count, pelvis_offset f32[3], yaw f32, rot f32[4×N].
  - A `PoseBone` enum fixes the order.
  - About 380 B, below the 64-try torn-read limit's comfort zone.

## Code to reuse (studied 2026-10-04; licenses checked)
- **Retarget core:** 2010-rust-rewrite-mashup `crates/render_anim/src/skate/rig.rs:22-149` (Apache-2.0, copy with credit).
  - Bone map with a child bone per segment.
  - Basis conversion `B·M·B⁻¹`.
  - Per-bone `fit` = `from_rotation_arc` between the two bind segments; `skin = posed·bind⁻¹·fit`.
  - Terminal bones reuse the parent's fit; unmapped bones inherit; neck/head locked to the parent.
  - The Rust side (`protocol/src/` or `er-plugin`) can use it close to as-is.
- **Quaternion helpers:** mashup `crates/anim_iw4/src/quat.rs:57-96` (Apache-2.0).
- **Ideas only, re-implement:** GTA San AnSkateas has no license on its own code. From `skate-ffi/src/lib.rs:698-1220` and `sa-plugin/src/main.cpp:853-960`:
  - facing derived from the hip/chest/foot body frames;
  - host bone lengths kept, hip height scaled by the leg ratio;
  - twist cap 0.35 rad and two-bone arm IK;
  - bind = inverse skin-to-bone;
  - writes as late as possible (just before render).
- **Interpolation:** SkyCraft `skse/src/Game.cpp:662-786` (MIT). It keeps a tick history, renders slightly in the past and never extrapolates. Killcraft `src/Host.cs:485-506` (MIT) sends prev/cur plus a time stamp. mashup `skate.rs:257-261` rejects non-finite poses.
- **Not found anywhere:** reading a Havok pose out of a running game. Step 1 is our own RE (no reference covers ER).

## Steps (each: build → test → user check when it needs the game → commit + push; stuck rule after 2 failed attempts per step)
1. **ER pose probe (ER only, research).** New `er-plugin/src/pose.rs` probe behind a cfg switch (`-ErProbe pose`).
   - Follow the 3 candidate pointers and scan for an hkArray (ptr, count 40–200) of 48-byte records with a unit quaternion and scale ≈ 1. Scan for an hkaSkeleton whose bone-name pointers read as ASCII.
   - Log the bone names once, then the pelvis/thigh quaternions every N frames during `-ErSelfTest dodge`.
   - **Accept:** a named bone list, and quaternions that change during rolls and are steady when idle. Findings go to `docs/research/elden-ring-pose.md`. Run with `tools/dev.ps1 -Target er -Game eldenring -Restart -WaitInWorld 20`; the user only presses Continue.
2. **Skyrim one-bone proof (Skyrim only, no ER).** New `skse/src/bridge/Pose.cpp`.
   - Once: walk `Get3D(false)` and log the bone names, and log the host bind from `NiSkinData`.
   - Debug key F9 (held) pitches the pelvis 45°. Try the 0xAD post-call first; if nothing changes on screen, use a vfunc 0x7D `UpdateAnimation` post-call hook in its own file.
   - **Accept:** the user sees the body tilt while F9 is held, in third person, standing and moving.
3. **Protocol v4 (no game).** Schema slot + `PoseBone` enum → protogen. A fake peer `er` writes a swinging pelvis/arm pose. Rust + C++ slot tests; `tests/run-tests.ps1` green.
4. **ER writer + math.**
   - Quaternion basis conversion and Δ math go in `protocol/src/coords.rs`, with unit tests (identity bind → identity Δ; known yaw → known yawFix).
   - `pose.rs` writes the slot every frame at `ChrIns_PostPhysics`, with `Active` from dodge/anim state.
5. **Skyrim applier.** All mapped bones, yawFix, pelvis offset (scaled by a height ratio), blend.
   - Test with the fake peer first: the user watches the pose swing.
   - Then both games: quick-tap Left Shift + W/A/S/D in combat. The roll should play in all 4 directions with no stutter, and frame times stay within today's numbers.
6. **Docs:** README "What works today" (roll animation), DESIGN §3/§4 (PoseState, authority), ROADMAP, MODLOG, STATUS, THIRD-PARTY-NOTICES (2010-rust-rewrite-mashup rig.rs if copied, Apache-2.0; credit the GTA San AnSkateas idea). Refresh `docs/ai/CLAUDE.md` at the milestone.

## Critical files
`er-plugin/src/{pose.rs (new), lib.rs, config.rs, combat.rs (dump helpers)}`, `protocol/schema/messages.toml` + `generated/*`, `protocol/src/coords.rs`, `tools/fake-peer`, `tests/run-tests.ps1`, `skse/src/bridge/{Pose.cpp (new), Bridge, Coords.h}`, `skse/src/hooks/{PlayerUpdate.cpp, AnimUpdate.cpp (new, only if needed)}`, `tools/dev.ps1` (`-ErProbe pose`).

## Verification
- `cargo test` for the quaternion/Δ math; `tests/run-tests.ps1` green before the protocol commit; `cargo run -p protogen -- --check` clean.
- Game runs only through `tools/dev.ps1`. User steps are in keyboard/mouse. I read the `[pose]` log lines (bone list, sampled quaternions, apply/blend state, frame-time p95/p99).
- Fail-safe: F10 off or ER killed → no pose writes within 250 ms (stale), and Skyrim's own animation returns.

## Risks
- The ER pose may not be found quickly (step 1 is pure RE). If so: stuck rule, hand off, try the Mtx43 palette (matrices without names, mapped by observation).
- The bind-pose shapes may differ (ER vs Skyrim T-pose arms). Fix: per-bone segment alignment (`from_rotation_arc` of the bind segments) as in San AnSkateas, added only if the arms look wrong.
