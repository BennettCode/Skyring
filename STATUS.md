# STATUS

_Last updated: 2026-10-05_

## Current phase
**P4: in progress** (plan: `docs/P4-PLAN.md`). Step 1 done (2026-10-04): while the bridge is on, Skyrim's vanilla sprint is off (Sprint =
ER dodge), F10 toggles the bridge, and the hidden ER character only gets the move stick around a dodge (stays parked).
Step 2 done: ER's combat flag found (`CSChrDataModule` +0x19a bit 0x40) and forced from code (rolls then cost stamina with no enemy).
Step 3 done: protocol v3; Skyrim's combat state drives ER's, so rolls cost stamina only while the Skyrim player fights.
Step 4 done: the Skyrim player follows ER's rolls (ER's velocity put into Skyrim's character controller every frame, 98–100 % of ER's distance, camera-relative,
chains, tap = dodge / hold = sprint, movement back at ER's move-cancel window). Step 4b: the hidden ER character is pinned to its spot
every frame and Skyrim follows its virtual position, so ER walls no longer shorten rolls (`er-plugin/src/park.rs`).
Step 5 done by pose streaming: ER's roll animation plays on the Skyrim player (`docs/POSE-PLAN.md`).
Step 6 done (2026-10-05): Skyrim's stamina bar shows ER's stamina share (`skse/src/bridge/Stamina.cpp`).
Step 7 done for melee (2026-10-05): NPC melee hits during ER's roll i-frames are dropped (`skse/src/hooks/PlayerHit.cpp`); arrows/spells → P5.
**P5 in progress** (`docs/P5-PLAN.md`): step 1 done (2026-10-05): ER swings hit Skyrim NPCs (hit window from ER memory, ER attack rating, ER-like damage, poise stagger; protocol v9). Step 2 done: the Skyrim weapon kind picks the ER weapon/moveset, material → upgrade level (protocol v10). Step 3 done: the player's health is ER's HP (protocol v11). Bows: ER draw/shot animation, Skyrim arrow on release (protocol v12).
Step 8 done (2026-10-05): DualSense with Elden Ring's layout (plugin reads it over HID; protocol v8 forwards R1/R2/L1/L2 to ER; ER ignores the physical pad while bridged).
LOCO-PLAN (`docs/LOCO-PLAN.md`): stage A (smooth, faithful rolls) and stage B (ER drives walking, running, sprinting and rolling while
the bridge is on, protocol v7) done. The run/sprint pose was checked bone by bone against ER and fixed (COM yaw, spine conjugation;
hip/chest/lean within 4° of ER), and the user's feel test raised no issue (2026-10-05).
P3 (done): ER runs hidden at 60 fps; Sprint (+ W/A/S/D) → ER backsteps/rolls by ER's rules; stamina, animation and the roll i-frame
window come back every frame; coordinate conversion measured (`protocol/src/coords.rs`). Region `Local\SkyrimXER_v7` (v4 PoseState, v5 stamps + PoseBind, v6 stance, v7 Locomote).

## What exists
- `protocol/`: schema `schema/messages.toml` (v3) → `tools/protogen` → `generated/skyrimxer_protocol.{h,rs}`. Rust crate `skyrimxer-protocol`
  (region, rings, seqlock slots, link state machine + `LinkShared` for game threads). C++ mirrors: `skse/src/bridge/{Link.*,Slot.h}`.
  Layout + link rules: `docs/DESIGN.md` §4.
- `skse/` → `SkyrimXER.dll` (CommonLibVR-ng 39f9d07, AE only). Logs load/kDataLoaded/save loaded. Link thread starts on kDataLoaded
  (`src/bridge/`), Bye on clean exit. Also builds `skyrimxer_link_test.exe` (C++ selftest + peer mode).
- `er-plugin/` → `skyrimxer_er.dll` (eldenring-rs 59fbd3b). Version check, FrameBegin task (frames, focus spoof, hides
  the window in-world, player state every 5 s), link thread. Dev switches in `build/er-plugin/skyrimxer_er.cfg` (written by `dev.ps1`).
- `tools/`: dev.ps1 (one-command loop; loads both games in by itself: ER confirm key, Skyrim auto-load, `-SkyrimKeys` key scripts via game-input.ps1), setup-check, build, deploy (+Undo/WhatIf), backup-saves, launch, collect-logs (prints [core]/[link]),
  protogen, fake-peer (`skyrim` pulses Dodge, `er` fakes a dodge into PlayerState). `tests/run-tests.ps1`: all protocol/link/slot tests,
  no game needed (≈ 25 s).
- Installed in the game (see `local/install-manifest.json` + `local/deploy-manifest.json`): SKSE 2.3.1, Address Library v13, Crash Logger, SkyrimXER.dll.

## Blockers / open questions
- **Skyrim + DualSense:** Skyrim sees no gamepad when started by skse64_loader (no Steam Input). Keyboard Sprint (Left Shift) is used for
  P3; controller support comes later (`docs/research/skyrim-hooks.md`).
- None blocking. Arena settled: the test character is parked in m10_01_00_00 and the user presses
  Continue each run (no warp in P3). The user tests with a DualSense (PS5) over USB in both games.

## Next 3 steps (P5, `docs/P5-PLAN.md`)
1. User playtest with the pad: hit sound, hit count, each weapon's moveset, getting hit / rolling through / dying.
2. ER armour decides incoming damage; poise/stagger on the player (ER poise vs NPC hits).
3. AR check against ER's own status screen; tuning from the playtest. (Also open: P4 accept playtest; ER freezes for seconds at load-in.)

## Handoff notes
**LOCO-PLAN stage B done, agent-tested (2026-10-05).** ER drives walking/running/sprinting/rolling while the bridge is on
(`skse/src/bridge/Locomotion.cpp`, protocol v7 `Locomote`, ER `remote.rs`). World offset W instead of a per-frame camera offset
(`docs/LOCO-PLAN.md`). Waiting for the user's feel test. Known gaps: NPCs see the actor's own heading, footsteps untested,
attacks/blocks/spells yield to vanilla until P5.

**LOCO-PLAN stage A, smooth player movement: solved (2026-10-05).** ApplyCurrent was refused on alternate frames, and writing
the controller's velocity or `velocityMod` after PlayerCharacter::Update didn't stick (Skyrim sets both again before the Havok step).
`hooks/ControllerVelocity.cpp` overrides `bhkCharProxyController::SetLinearVelocityImpl` (vtable [1], AE 240560) for the player:
0 stalls, 98–100 % distance, a wall still stops the player. Notes: `docs/research/skyrim-hooks.md`.

**P4 step 5, roll animation: vanilla route stopped after 3 attempts (2026-10-04, stuck rule).** Attempt 3 (Sneak through the game's
SneakHandler, forward move input, Silent Roll perk added while bridged; `skse/src/bridge/VanillaInput.cpp`, `kSneakRollTrick` now off):
the roll plays and sneak toggles cleanly, but **only forward**: the third-person camera re-aligns the body to the camera yaw every frame
(SetHeading + ThirdPersonState free rotation ignored). User, asked how the other merged games do it: GTA San AnSkateas **streams the hidden
engine's bone pose onto the host character each frame** (`reference/GTA-San-AnSkateas`, `mashup/docs/SKATE.md`: `render_anim/src/skate/rig.rs`
"maps Skate 3 bones onto the soldier skeleton"; no animation files converted or shipped). SkyCraft/FalloutCraft render the hidden game's
pieces offscreen and composite them (our P8 stretch). **Recommended next (fresh chat, plan mode):** ER pose streaming. Research: where ER
keeps the hidden character's final bone pose (eldenring-rs ChrIns has only opaque `hka_pose_importer` / `anim_skeleton_to_model_modifier`
pointers, chr_ins.rs ~297), ER bone names vs Skyrim's (`NPC Pelvis`, `NPC Spine` ...), a per-frame protocol slot (~30 bones × quaternion),
and the Skyrim write point after its animation update (NiNode local rotations + world update). Prove with one bone first.
Alternatives: a dodge animation mod (TK Dodge RE + Pandora/Nemesis), or keep the slide.

**P4 step 5, attempts 1–2 (2026-10-04).** Goal: a roll animation in Skyrim with vanilla files only.
Tried: (1) `NotifyAnimationGraph("SneakSprintStartRoll")` → returns true, nothing plays. (2) Sneak trick in `Movement.cpp`
(`kSneakRollTrick`, now off): SneakStart, SprintStart one frame later → the roll starts but is cut after ~8 frames (we zero
`moveInputVec` during dodges, so the sprint ends), half the tries only crouch, the sneak eye stays on the HUD afterwards.
Hypothesis: the sneak-sprint roll needs forward move input for its ~29 frames. Alternatives: (a) keep `moveInputVec` = (0,1) during
the trick and leave sneak through the game's own sneak toggle (not the actorState bit); (b) play the roll another way (a vanilla
IdleForm / `PlayAnimation`); (c) user-installed dodge animations once players accept an animation dependency. Logs: `[anim]` probe
lines (`bridge/AnimProbe.cpp`), `[move] roll animation` lines.

**I-frame search solved (2026-10-04, attempt 3).** Attempts 1–2 diffed memory only during *backsteps*, which set no invincibility at
all. A roll sets `action_modifiers_flags` bit 1 for 26–27 frames; a play test with 46 logged hits had none land while it was set.
`game.rs` publishes it as `PlayerFlag::IFrame`. The Watcher's `[probe] HIT` lines (every HP loss with bits + frame offset into the last
dodge) stay as a tool for P4/P5.

**P3 step 3 done (2026-10-04).** Protocol v2 slots are in, and the tests/fake peers exercise them. The plugins were only rebuilt against v2
(no behaviour change). Step 4 hands `Link::shared()` / `Link::Shared()` to the game threads and moves the peers' `PlayerWatch` edge
logger into the Skyrim plugin.

**P3 step 2 done (2026-10-04, session 3).** Hidden ER backsteps from code: hold `Backstep` (virtual 270) + `BackstepTapped` (virtual 269)
on the press frame only, written at WorldChrMan_Prepare, with the focus spoof run at FrameBegin and again at WorldChrMan_Prepare.
Open note: the i-frame/dodging flags read 0 during backsteps (real ones too); check before PlayerState uses them.
Details: `docs/research/elden-ring-input.md` ("session 3").
