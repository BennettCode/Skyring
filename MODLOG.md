# MODLOG

Newest first. One entry per session or verified step: **what changed · how it was tested · result · follow-ups**.

---

## 2026-10-04: POSE-PLAN step 2, Skyrim one-bone proof (Skyrim only)
- **Changed:**
  - `skse/src/bridge/Pose.{h,cpp}` (new): logs the third-person bone tree and each skinned geometry's bones (the bind-pose source) once. While F7 is held it turns the pelvis 45° right after `PlayerCharacter::Update`, using `local.rotate` + `UpdateDownwardPass`, and traces whether the write is still there at the next Update.
  - `bridge/Input`: `KeyHeld(scan code)`. `hooks/PlayerUpdate.cpp` calls the pose code before and after the original Update.
- **Tested:** Skyrim, keyboard. User: "F7 brought the legs out". A write after Update shows on screen; the animation re-poses the skeleton before the next Update.
  - A trial `UpdateAnimation` (vfunc 0x7D) hook showed it runs on a worker thread, once per frame. The hook was removed again.
  - `tests/run-tests.ps1` green.
- **Result:** ✔ step 2 accepted. Notes: `docs/research/skyrim-hooks.md` "Posing the player's skeleton". Also checked SkyCraft-SkateBridge and modern-warfare-2-ai (Skyrim + Skate plans). Neither has pose code yet; both plan the same `rig.rs` retarget we use. Next: step 3, protocol v4 PoseState.

## 2026-10-04: POSE-PLAN step 1, ER skeleton pose found (ER only)
- **Changed:**
  - `er-plugin/src/pose.rs` (new, research): `pose_probe=1` / `tools/dev.ps1 -ErPoseProbe`. A one-time object-graph walk from the opaque pose pointers, using safe reads (ReadProcessMemory on the own process) and RTTI names. It detects transform and bone-name arrays, then samples them during rolls, and logs the parents, the bind pose and the key bones.
  - Cargo: windows features `Win32_System_Diagnostics_Debug`, `Win32_System_Threading`.
- **Tested:** 2 ER runs with the roll self-test (user pressed Continue). Found `ChrIns+0x398` → `CSFD4LocationHkaPoseImporter` → hkaPose: skeleton (150 named bones, parents, bind) plus local and model pose. During a roll all model bones move (pelvis 0.94 → 0.27 m); idle stays under 1°. Notes: `docs/research/elden-ring-pose.md`.
- **Result:** ✔ step 1 accepted. Next: step 2, the Skyrim one-bone proof.

## 2026-10-04: Pose-streaming plan + reference study (docs only)
- **Changed:**
  - `docs/POSE-PLAN.md` (new): plan for P4 step 5. ER plays the roll on the hidden character; its bone pose is retargeted onto the Skyrim player every frame. The plan has 6 steps and starts with an ER pose memory probe. P4-PLAN step 5 links to it.
  - Studied Killcraft and 2010-rust-rewrite-mashup next to the existing references. THIRD-PARTY-NOTICES credits both.
  - The AI workflow doc (`docs/ai/CLAUDE.md` §14) now lists, for each problem (pose, interpolation, hits, HUD, camera, input, debug tools), where the reference projects solved it, plus a "when stuck" checklist.
- **Tested:** n/a (docs). Cited reference files checked to exist.
- **Result:** ✔ plan approved in direction by the user. Next: POSE-PLAN step 1 (ER pose probe).

## 2026-10-04: P4 step 5, attempt 3: vanilla Silent Roll (off; handed off)
- **Changed:** `skse/src/bridge/VanillaInput.{h,cpp}` (new): Sneak pressed through the game's SneakHandler with a synthetic ButtonEvent;
  Silent Roll perk added while the bridge is on (removed on F10 if added). `Movement.cpp`: trick = Sneak press → SprintStart once sneaking
  (retried) → forward move input for 0.48 s → sprint off + Sneak press; body turn via SetHeading + camera free rotation; chained rolls keep
  sneak; the "press + ER moving" chain rule only for backsteps (it falsely restarted dodges in a roll's recovery). `kSneakRollTrick` = false.
- **Tested:** both games, keyboard, 2 runs (~45 rolls). Sneak toggles cleanly, the roll plays, distances 98–101 %; but only forward (the
  third-person camera re-aligns the body each frame), and run 1's weapon-drawn rolls got no SprintStart. User: "it isn't working".
  `tests/run-tests.ps1` green.
- **Result:** ✖ vanilla route stopped (3 attempts). Next: stream ER's bone pose onto the Skyrim skeleton (STATUS handoff), user's direction
  ("do what the other merged games do").

## 2026-10-04: P4 step 4b: the hidden ER character is pinned (ER only)
- **Changed:** `er-plugin/src/park.rs` rewritten: every frame (ChrIns_PostPhysics) the character's horizontal step is added to a virtual
  position and the character is put back on its spot (physics position + `chr_proxy_pos_update_requested`); PlayerState.pos = the virtual
  position (`remote.rs`, same task as the pin in `lib.rs`). A jump > 1.5 m in one frame = new spot. `pin` config key / dev.ps1 `-ErNoPin`.
  `[park] dodge` line per dodge (virtual distance, max drift). dev.ps1 deletes the old ER log before launching (a just-killed ER's
  "in world" line ended `-WaitInWorld` runs early).
- **Tested:** ER self-test A/B: free 3.09/3.51/2.19/**0.07**/3.51/**0.08**/3.30/3.51 m (wall), pinned 3.51 m ×8, max drift 0.17 m,
  i-frames 27 frames. Both games (keyboard, W/A/S/D + 10 chained): 40+ rolls at 92–100 % of ER's distance, ER real position constant.
  `tests/run-tests.ps1` green.
- **Result:** ✔ user request "the ER character must not move unless rolling" (it doesn't move at all now). Skyrim unchanged.

## 2026-10-04: P4 step 4: the Skyrim player follows ER's rolls (both games)
- **Changed:** `skse/src/bridge/Movement.cpp` (new): while ER plays a dodge, ER's per-frame delta → character frame → Skyrim along the
  roll direction (camera yaw + move keys), applied closed-loop through `Actor::ApplyCurrent`; ends at ER's TAE movement-cancel window
  (new PlayerFlag MoveCancel, ER `game.rs`) or when ER stops; chained rolls/backsteps detected. `hooks/MoveSwallow.cpp`
  (MovementHandler::CanProcess, AE 208715): movement keys off during a dodge. Tap = dodge / hold = sprint: ER drops the stick after
  20 frames (`remote.rs` DASH_AFTER), Skyrim lets its sprint through as a fresh press (`SprintSwallow.cpp`). ER `park.rs`: the hidden
  character returns to its spot after each dodge. `bridge/AnimProbe.cpp`: bounded `[anim]` graph-event log. Roll-animation experiments
  (step 5) are in `Movement.cpp`, switched off.
- **Tested:** both games, keyboard, ~10 user runs. Final: rolls 96–99 % of ER's distance, a wall stops the roll, camera calm, rolls follow
  the camera, spam chains, backsteps chain, Shift hold sprints at ~510 u/s (vanilla sprint), the hidden character snaps back.
  `tests/run-tests.ps1` green (MoveCancel enum).
- **Result:** ✔ step 4. Step 5 (animation) stuck after 2 attempts, handoff in STATUS. Open: ER walls still shorten some rolls.

## 2026-10-04: P4 step 3: protocol v3, Skyrim's combat state drives ER's
- **Changed:** protocol v3 (`Local\SkyrimXER_v3`): InputState `_pad1` → `flags` (InputFlag InCombat, BridgeOn), PlayerFlag InCombat.
  Skyrim `Bridge.cpp` writes them (`IsInCombat()`, F10) and logs `[combat]` edges of both games. ER: `remote.rs` hands the wanted state to
  `combat::MirrorCombat` (ChrIns_AILogic, every frame, both ways); bridge off or stale input = ER decides. PlayerState reports ER's state.
- **Tested:** `tests/run-tests.ps1` green. Both games (keyboard): out of combat 3/3 rolls free; NPC set hostile (`startcombat player`)
  → 4/4 rolls cost 12 stamina; after `kill` 3/3 free again; all rolls with i-frames. Holding Sprint dashes (ER rule) and drains stamina
  in combat only.
- **Result:** ✔ step 3. Next: step 4, the Skyrim player follows the roll.

## 2026-10-04: P4 step 2: ER combat flag found and forced (ER self-test)
- **Changed:** `er-plugin/src/combat.rs`: `in_combat`/`set_in_combat` (CSChrDataModule +0x19a bit 0x40 = out of combat), `CombatWatch`
  (logs ER's combat-state edges every session), `ForceCombat` (`-ErForceCombat on|off`, one write per frame in ChrIns_AILogic), research
  tools behind `-ErDump`: `Dump` (raw PlayerIns/PlayerGameData/module snapshots → `logs/combat_dump.bin`) and `SpEffectWatch`.
  `log.rs` remembers the log dir; `config.rs` + `tools/dev.ps1` got the new switches.
- **Tested:** (1) SpEffect watch while the user rolled calm/in combat: no SpEffect explains it (ruled out). (2) Memory dump of the same
  sequence, analysed offline: one bit separates 7 free from 8 costly rolls. (3) Self-tests: written in every group, only ChrIns_NaviCache
  changes it back; forced in combat with no enemy, 5/5 rolls cost 12 stamina. The final single-group write (AILogic) follows from (3)
  and gets re-checked in step 3's both-games test.
- **Result:** ✔ step 2. Next: step 3, protocol v3 carries Skyrim's combat state to ER.

## 2026-10-04: P4 step 1: Sprint swallowed, F10 toggle, ER stick only around dodges
- **Changed:** P4 plan (`docs/P4-PLAN.md`, ROADMAP P4/P5 edited after the user's choices). New `skse/src/hooks/SprintSwallow.cpp`:
  `SprintHandler::CanProcess` (vtable AE 208717, vfunc 0x1) refuses Sprint presses while `bridge::SwallowSprint()` (bridge on + connected +
  ER in world); releases pass. `Input.cpp`: F10 toggles the bridge (off = nothing forwarded or swallowed), HUD message. ER `remote.rs`: the
  move stick is forwarded only while Dodge is held and 10 frames after, so the hidden character no longer walks with Skyrim.
- **Tested:** both games, keyboard (user). Bridged Shift+W ≈ 366 u/s (plain run 356), bridge off ≈ 499 (vanilla sprint); F10 both ways;
  plain walks sent nothing to ER; 5/5 directional rolls with i-frames.
- **Result:** ✔ step 1. Next: step 2, ER combat state (rolls free out of combat).

## 2026-10-04: Docs: "Made with Claude" + public CLAUDE.md
- **Changed:** README "Made with Claude" section (built with Claude Opus 5.5 in Claude Code, guided by a project CLAUDE.md) linking a new
  public copy `docs/ai/CLAUDE.md` (the project's agent instructions, cleaned of machine paths and private notes; copy it to the repo root to use it).
  Repository layout + Legal lines mention it.
- **Tested:** docs only; scanned the copy for private strings.

## 2026-10-04: P3: coordinate/yaw conversion measured (P3 done)
- **Changed:** bounded `[coords]` samples on both sides (Skyrim `Bridge.cpp` `SampleCoords`: position + `GetAngleZ()`; ER `remote.rs`
  `sample_coords` in ChrIns_PostPhysics: position + yaw + move), max 1500 lines per session, only while moving. New `protocol/src/coords.rs`
  (Local forward/right/up, ER/Skyrim delta ⇄ local, yaw delta) and its C++ mirror `skse/src/bridge/Coords.h`; unit tests in both languages
  with the measured segments. `bridge::OnFrame` now gets the player pointer from the hook.
- **Tested:** both games, the user walked W/D/S/A ~3 s each. Skyrim: W 2.7°, D 90.1°, S 176.5° off the heading (A hit an obstacle).
  ER: 63/63 fast samples run at yaw + 180°; W→D turned +90°, later facings predicted to 0.1° with "right = yaw + 90°". `tests/run-tests.ps1` green.
- **Result:** ✔ conventions in DESIGN §6 / `docs/research/coordinates.md`. **P3 done.** Next: P4 plan.

## 2026-10-04: P3: roll direction: Skyrim W/A/S/D → ER move stick (P3 accept met)
- **Changed:** `er-plugin/src/pad.rs`: analog slot helpers (`analog_slots`, `set_analog`, `poll_analog`, `set_move`, `move_polls`).
  `remote.rs`: fresh `move_x/move_y` from Skyrim → ER's MoveForwards/Backwards/Left/Right analog slots every frame while held, one release
  write, logged on change. `actions.rs`: `selftest=roll` (direction cycled per pulse, held 10 frames before the press until 5 after the release),
  the watch line shows the polled stick. Skyrim `bridge/Input.cpp`: Forward/Back/Strafe Left/Strafe Right user events → `MoveAxes()`;
  `Bridge.cpp` writes them into InputState and logs changes. `tools/dev.ps1 -ErSelfTest roll`. No protocol change (fields were in v2).
- **Tested:** ER self-test 1: forward/right rolled, back/left only backstepped (positive values ignored there). Self-test 2 with negative
  Backwards/Left: 7/7 short taps in all four directions roll (27110, dodge flag 27 frames). Both games (user, keyboard): 8/8 Sprint + direction
  → roll, Skyrim `IFrame on` 34–51 ms after the release and off ~450 ms later, HUD "i-frames yes"; 2/2 Sprint alone → backstep, "i-frames no".
  `tests/run-tests.ps1` green.
- **Result:** ✔ P3 accept met (stamina drop in combat earlier, i-frame window now). Next: coordinate/yaw test, the last P3 item.

## 2026-10-04: P3: i-frame window found (rolls set FLAG_AS_DODGING)
- **Changed:** `er-plugin/src/game.rs`: `iframe` = `action_modifiers_flags` bit 1 (`dodging`, TAE FLAG_AS_DODGING) or an invincibility bit
  (0/3/5), was bit 0 only. `actions.rs`: the Watcher logs HP, opens a watch window on every dodge animation (real presses too), and logs every HP
  loss with the modifier bits and the frame offset into the last dodge (`[probe] HIT`). Word-diff probe trimmed to the action-flag module.
- **Tested in-game (ER only):** (1) self-test backsteps out of combat with a special-effect diff and a per-task-group sample of the modifier bits:
  no effect and no bit during backsteps. (2) Combat run: a backstep-while-walking became a roll and showed bit 1 for ~27 frames.
  (3) User play test, ~20 rolls + some backsteps against an enemy: bit 1 set for 26–27 frames from each roll's first frame, **0 of 46 hits
  landed while it was set**; backsteps set nothing and were hit at +1..+34 frames. `tests/run-tests.ps1` green.
- **Result:** ✔ the i-frame source is found and published as `PlayerFlag::IFrame`. Skyrim's Sprint injects a backstep (no i-frames), so the
  both-games IFrame check moves to step 5 (roll direction). Details: `docs/research/elden-ring-state.md`.

## 2026-10-04: P3: i-frame search attempt 2 (not found) → handoff
- **Changed:** `er-plugin/src/actions.rs` Watcher: the dev-only word-diff probe now covers three regions (`action_flag`, `event` = CSChrEventModule,
  `chr_flags` = ChrIns+0x1c4..0x1cb), and the per-frame `[action]` line shows raw `ev_flags`.
- **Tested in-game (ER only, self-test, 10 backsteps):** no region shows an i-frame window. `ev_flags` is a constant 0xff; the chr_flags bits flip
  irregularly. Details: `docs/research/elden-ring-state.md`.
- **Result:** ✘ second failed attempt at the same problem → stuck-loop rule: handoff written (STATUS "Handoff notes"); the next try runs in a fresh session.

## 2026-10-04: Docs: README + roadmap refresh for the GitHub page
- **Changed:** README: author's "About this project" section, "What works today" / "Not working yet", input badge, Tested setup says
  keyboard + mouse (**controller support not added yet**, why, and that it's planned for P4), dev-loop commands updated. ROADMAP: P3
  ticks match reality, controller support added to P4, installer added to P8, new "Beyond the roadmap: ideas" section.
- **Tested:** docs only.

## 2026-10-04: P3: stamina drop shows in Skyrim (combat); torn-read fix; regen log squashed
- **Tested in-game (both games, ER visible, enemy aggroed in m60_42_37_00):** Left Shift in Skyrim → ER backstep 12027010 → Skyrim log + HUD
  `stamina 136→128` (a backstep costs 8 stamina in combat; out of combat it costs 0, which is why earlier runs showed none). The enemy's hits showed as
  HP 1450→1186. **I-frame flag still not found:** the action-flag word diff in combat shows only the anim-length markers (+0x10 bit 0,
  +0x40 bit 15 `disable_turning`, +0x1d8) and a combat bit (+0x10 bit 4).
- **Fixed:** a slot read can fail when the writer is pre-empted mid-write (all 64 tries torn). `run-tests` caught the watcher logging a false
  `stale` because of this. All readers (PlayerWatch C++/Rust, ER `DodgeFromSkyrim`, fake ER) now keep the last good copy and judge staleness by its age;
  before, ER would also have released a held Dodge for a frame. Stamina regen is now one `stamina regen A→B` line instead of one line per frame.
- **Tested:** `tests/run-tests.ps1` green 3× in a row; both plugins build.
- **Result:** the P3 accept's stamina part ✔ (with timestamps). The i-frame window is still open.

## 2026-10-04: P3: Skyrim HUD feedback for playtests
- **Changed:** `skse/src/bridge/Hud.cpp` (ShowHUDMessage, AE id 52933, checked with addrlib-check). `Bridge.cpp` `UpdateHud`: top-left
  notifications for ER connected/lost, ER character in/out of the world, and one summary per Dodge press 0.6 s later (ER anim reaction,
  stamina before→lowest, i-frames seen). Each one is also logged as `[hud]`.
- **Tested in-game (both games, keyboard):** connected + in-world messages appeared; 6 Left Shift taps → 6 × `ER dodge: anim 12027010 |
  stamina 136->136 | i-frames no` (backstep in animation group 12; the character's stance changed since step 2, idle = 12000000).
- **Result:** ✔. Still open: stamina/i-frames need combat (no enemy aggroed in these runs).

## 2026-10-04: P3 step 4b: the loop runs in both games (Skyrim Sprint → hidden ER backstep → state back)
- **Changed:** `er-plugin/src/remote.rs`: `DodgeFromSkyrim` (in WorldChrMan_Prepare, replaces the self-test unless `selftest=dodge`) holds
  Backstep (+ BackstepTapped on the press frame) while InputState is fresh + connected + Dodge held; it releases on stale/lost.
  `publish_state` writes PlayerState every frame (ChrIns_PostPhysics in world, FrameBegin while not in world). `bridge::shared()`.
  `actions.rs`: the watch window now always runs, plus a bounded word-diff probe of the action-flag module (i-frame search).
- **Tested in-game:** (1) real ER (hidden) + `fake-peer skyrim --dodge-every 4`: every press after spawn-in backstepped (anim 27010 +9 frames);
  fake-peer killed mid-hold → ER `InputState stale` 266 ms later + release, then the link timeout. (2) **Both games** (keyboard in
  Skyrim): Left Shift tap → `[ER] Dodge down` 9 ms later → `[SKY] anim 0→27010` ~170 ms after the press, for 3 taps + a hold;
  both games at 60 fps (Skyrim p50 17.0 ms). `tests/run-tests.ps1` passes.
- **Not shown yet:** stamina stays 101 and no i-frame flag gets set (real backsteps behave the same). Findings: `docs/research/elden-ring-state.md`.
- **Result:** step 4 loop ✔. The P3 accept's stamina-drop + i-frame part is still open.

## 2026-10-04: P3 step 4a: Skyrim frame hook + Sprint → InputState (vs fake ER)
- **Changed:** `skse/src/hooks/PlayerUpdate.cpp` (PlayerCharacter::Update vfunc 0xAD), `skse/src/bridge/Input.cpp` (input sink: Sprint
  user event held → Dodge; logs the first event per device and each user event once), `Bridge.cpp` `OnFrame` (writes InputState, reads
  PlayerState through `PlayerWatch.h`, `[perf]` p50/p95/p99 every 5 s, frames now in Heartbeat). `tools/addrlib-check.ps1` checks
  Address Library ids. `launch.ps1 -SkyrimVia steam|loader` (default loader).
- **Tested in-game (Skyrim + `fake-peer er`, keyboard):** the ids were checked first (all present). Sprint (Left Shift) taps → `Dodge down` →
  PlayerState stamina 100→80 + IFrame on ~17 ms later, IFrame off after 1 s, regen; a hold works as well. Frame time p50 16.8 ms, hook ≈ 2 µs.
  Clean exit → fake ER saw Bye. `tests/run-tests.ps1` passes.
- **Found:** Skyrim gets **no gamepad events** when started by skse64_loader outside Steam (no Steam Input). The Steam launch option
  `"...skse64_loader.exe" %command%` fails ("too many free args"). The controller is deferred (user decision). Notes: `docs/research/skyrim-hooks.md`.
- **Result:** step 4 Skyrim side ✔ (keyboard). Next: ER side (injector from InputState, PlayerState publisher).

## 2026-10-04: P3 step 3 done: protocol v2 seqlock slots
- **Changed:** schema v2 (`Local\SkyrimXER_v2`): `InputState` slot @0x200 (sky→er), `PlayerState` slot @0x300 (er→sky), `Button`/`PlayerFlag`
  bit enums, `SLOT_STALE_MS`/`SLOT_READ_TRIES`. protogen requires `SLOT_*` structs to start with `seq: u32`. New `protocol/src/slot.rs` + C++ mirror
  `skse/src/bridge/Slot.h` (seqlock, body copied as u32 atomics). `LinkShared` (region + `connected`) on both links for lock-free game-thread access,
  published only after a valid join. fake-peer + C++ peer: link thread + frame loop, Dodge pulses (`--dodge-every`), fake ER dodge model, PlayerState edge log.
- **Tested (no game):** `tests/run-tests.ps1` all green. Covered: protogen (incl. the slot rule); Rust slot tests (round trip, dead-writer repair,
  fresh, 0.5 s concurrent torn-read check); link `LinkShared` asserts; the C++ selftest (same + a threaded torn-read check, ~9 M reads); interop A
  (C++ Skyrim Dodge → Rust ER dodges → C++ sees stamina 100→80 and IFrame on/off ~31 ms later, stale once after the crash); interop B. Both
  plugins rebuilt + deployed (`dev.ps1 -NoLaunch`).
- **Result:** P3 step 3 ✔. Next: step 4 (wire the loop in both games).

## 2026-10-04: P3 step 2 done: injected backstep works with ER hidden
- **Changed:** `er-plugin/src/window.rs`: the focus spoof also runs at WorldChrMan_Prepare (PadStep re-sets `is_back_ground_window`
  every frame while ER isn't foreground). `er-plugin/src/focus.rs`: change-only focus-flag probe (dev, `probe=1`).
- **Tested in-game (ER only):** focus probe run (visible, user switched windows): backsteps stopped while another window was foreground,
  `is_back_ground_window` was 1 after PadStep. After the fix, hidden run, hands off: 7/8 pulses backstepped (the miss was during spawn-in), 60 fps.
  `tests/run-tests.ps1` passes.
- **Result:** P3 step 2 ✔. Open note: i-frame/dodging flags read 0 during backsteps (real ones too), see research note.

## 2026-10-04: P3 step 2: injected backstep works (window focused)
- **Changed:** `er-plugin/src/pad.rs` (`describe`, `poll_mask`, `layers`: read-only input diagnostics), `actions.rs` (wide per-frame probe:
  full action masks, all key polls, device layers; self-test also raises `BackstepTapped` on the press frame; holds 4/9/30). `tools/dev.ps1`
  refuses to launch ER without Steam running.
- **Tested in-game (ER only, window visible, 2 runs):** probe compared a real DualSense tap with ours: the real one also raises
  `BackstepTapped` for one frame, which makes the release a backstep request (bit 16). With that added, the self-test backsteps
  (anim 27010) on its own; the user saw it. `tests/run-tests.ps1` passes.
- **Result:** step 2 ✔ while ER is focused. Still open: the action module ignores the pad while ER isn't the foreground window
  (the hidden case). Details: `docs/research/elden-ring-input.md` ("session 3").

## 2026-10-04: P3 step 2 diagnostics (dodge injection, still blocked)
- **Changed:** `er-plugin/src/actions.rs`: the probe logs the action-request gating masks every 120 frames (possible, cancels, disabled, queued,
  animation flags) and a queued bit per task group. The self-test cycles hold lengths 4/8/12/20/30 frames and logs the engine's hold timer. Dev-only, off by default.
- **Tested in-game (ER only, 3 runs, window visible):** the virtual press reaches the action-request module like a real press; no dodge for any hold
  length; a real DualSense tap in the same session backsteps with identical module values.
- **Result:** step 2 still ✘, narrowed: the module isn't the missing link. Findings in `docs/research/elden-ring-input.md` ("session 2 results").
  Also: me3 needs Steam running (`Steam is required to run this game` in the me3 log).

## 2026-10-04: Phase 3 started: ER runs hidden; dodge injection research
- **Changed:** P3 plan (`docs/P3-PLAN.md`). `er-plugin`: `window.rs` (each frame: focus flags spoofed; hide the window while the player is in
  the world, show it again at the title), `game.rs` (player snapshot: HP/FP/stamina, anim, i-frame/dodging/hyperarmor flags, poise, position,
  yaw, map), `config.rs` (`skyrimxer_er.cfg` next to the DLL), `actions.rs` + `pad.rs` (dodge self-test + frame-phase probe, dev-only, off by default).
  `tools/dev.ps1`: `-Restart`, `-ErVisible`, `-ErSelfTest`, `-ErInjectGroup`, `-ErProbe`. `tools/build.ps1`: retries the DLL copy after a kill.
- **Tested in-game (ER only):** the window hides on load-in and ER keeps running at 60 fps hidden; the state line reads stamina 101/101, HP 455/455,
  map m10_01_00_00. Probe of a real dodge press recorded where ER sets its action bits (`docs/research/elden-ring-input.md`).
- **Result:** step 1 ✔. Step 2 (injected dodge) ✘ so far: writing `action_requests` is overwritten by the engine; a held virtual pad key is seen
  by ER's pad poll but doesn't reach the player's action requests. Next hypotheses are in the research note.
- **Workflow pass (same day):** README rewritten (diagram, progress table, tested setup incl. **DualSense over USB** as the only test controller,
  safety section, one-command dev table). `dev.ps1 -WaitInWorld N` waits for the load-in and prints the ER results itself;
  `tools/stop-games.ps1` (+ shared `Stop-Game` in `common.psm1`); `tools/README.md` refreshed. DESIGN §9: controller open question.
  Tested: scripts parse, `dev.ps1 -Target er -NoLaunch` OK, `stop-games.ps1` closed a hidden ER.

## 2026-10-04: Phase 2 complete (shared memory + heartbeat)
- **Changed:** Protocol schema v1 (`protocol/schema/messages.toml`) + generator `tools/protogen` (rejects implicit padding, checks the region
  map, emits size/offset asserts for C++ and Rust, `--check` stale test). Rust crate `skyrimxer-protocol` (region, SPSC rings, link state
  machine) used by `er-plugin` and `tools/fake-peer`. C++ mirror `skse/src/bridge/Link.cpp` + `Bridge.cpp`, and `skyrimxer_link_test.exe`.
  `tests/run-tests.ps1`. `collect-logs.ps1` now prints `[link]` lines (heartbeat lines summarised).
- **Tested without games:** `tests/run-tests.ps1` passes: 10 Rust tests (ring wrap/full/corrupt, handshake, timeout, reconnect, Bye, restart,
  header mismatch), 9 C++ selftest checks, Rust↔C++ cross-process (crash → timeout; clean exit → Bye).
- **Tested in-game (agent-run, both games at the main menu):**
  - ER created the region, Skyrim opened it on kDataLoaded: `CONNECTED: handshake ok` on both sides within 50 ms.
  - ~7 min connected: heartbeat events every 5 s both ways, 0 dropped. ER `frames` rose ~300 per 5 s (≈ 60 fps while unfocused at the title).
  - `eldenring.exe` force-killed → Skyrim: `LOST: ER ... heartbeat timeout (last beat 2031 ms ago)`, Skyrim kept running.
  - ER relaunched alone → `attach#2`, reconnected without restarting Skyrim.
  - Skyrim window closed → clean exit, ER: `LOST: Skyrim ... shutting down` + `Skyrim said Bye (reason=Quit)`, ER kept running. No crash logs.
- **Bugs fixed during the work:** Hello read before its sender was detected looked like a restart (fix: check peer before reading events, and
  answer a Hello received while connected). ER log level name `warn` → `warning` to match spdlog.
- **Next:** P3.

## 2026-10-04: Workflow + repo hygiene
- **Changed:** Added `tools/dev.ps1` (build → deploy → backup → launch → wait for plugin ready lines → collect logs). AI-agent files are now
  local-only, and history was rewritten so they never appear in it. Logging format moved to `docs/DESIGN.md` §8 and the release checklist to
  `release/README.md`, so public docs are self-contained. Added a local pre-commit guard against private data and game/binary files.
- **Tested:** `dev.ps1 -NoLaunch` (full build + deploy in 9 s). The pre-commit guard blocks a staged Windows user path and passes clean files.
  The full `dev.ps1` launch path gets its first real run in P2.

## 2026-10-04: Phase 1 complete (hello world from both plugins)
- **Changed:** Added `skse/` (CMake + Ninja preset, VS-bundled vcpkg pinned to registry baseline 00c5775, CommonLibVR-ng submodule @39f9d07,
  AE only, `src/main.cpp`). Added the Cargo workspace + `er-plugin/` (eldenring-rs @59fbd3b, `lib.rs` + `log.rs`, panic=unwind).
  Added tools: build, deploy, backup-saves, launch, collect-logs, common.psm1. Added `local/paths.json`.
- **Tested in-game (both games at once via `tools/launch.ps1`):**
  - Skyrim: `SkyrimXER v0.1.0.0 loaded, runtime 1.7.104.0` → `kDataLoaded`. skse64.log: "loaded correctly".
  - ER: `loaded` → `game version supported, task system ready` → `per-frame task registered` → user loaded the test character →
    `main player spawned (in world)` → `main player gone (menu/loading)`. me3 attach config: our native, `savefile: skyrimxer.sl2`, `start_online: false`.
- **Bugs fixed:** (1) `SKSE::Init` replaced our logger, fixed with `SKSE::InitInfo{ .log = false }`, re-tested OK. (2) build.ps1 aborted on the
  VS dev shell's harmless stderr under PS 5.1 + `Stop`, fixed by running that call with `Continue` and checking the exit code.
- **Build times:** SKSE cold 385 s / incremental 10 s. ER cold 4m39s / incremental ~2 s.
- **Next:** P2 shared memory + heartbeat.

## 2026-10-03: Phase 0 complete
- **Decided:** CommonLibVR-ng 10.1.0 (`ae` preset; has `RUNTIME_SSE_1_7_104`). eldenring-rs v0.14 (supports exe 2.7.1.0 WW). libER dropped.
  Project license **GPL-3.0-or-later** (user choice; every CommonLib supporting 1.7.104 is GPL-3.0). `LICENSE` added.
- **Found:** ER input can be injected as `ChrActions` bits on `CSChrActionRequestModule` (no OS keys). Focus flag is
  `DLUserInputManagerImpl.is_game_window_focused`. Notes in `docs/research/elden-ring-input.md`.
- **Tested:** eldenring-rs `apply-speffect` example built (29 s). CommonLib `ae` built with VS 2026 + bundled vcpkg (590 s), unit tests 25/25.
  `tools/setup-check.ps1` written and passing.
- **Moved to P3:** arena choice, movement-direction source, both-games-at-once performance.

## 2026-10-03: me3 smoke test (Elden Ring, no mods)
- **Ran:** `me3 launch -g eldenring --savefile skyrimxer.sl2` after backing up ER saves to `local/save-backups/`.
- **Result ✔:** me3 attached to "ELDEN RING 1.17.1.0 Worldwide". Hooks applied (filesystem, allocators, assets, skip_logos). Arxan detected
  and the attach deferred cleanly. No EAC process. All save I/O redirected to `skyrimxer.sl2` (a copy of ER0000 made on first use), and
  `ER0000.sl2` was untouched. Zero WARN/ERROR lines. Log: `logs/me3-smoke-stderr.txt` + `%LOCALAPPDATA%\garyttierney\me3\data\logs\transient-profile\`.
- **Next:** Phase 0 library checks (CommonLib fork for 1.7.104, eldenring-rs for 1.17.1).

## 2026-10-03: Tooling installed
- **Changed:** Installed me3 0.13.0 (official signed installer, silent). Installed Crash Logger 1.25.0 (GitHub) into `Data\SKSE\Plugins`.
  Found that the installed SKSE 2.2.6 targeted 1.6.1170, which doesn't match the game's 1.7.104. Replaced it with **SKSE 2.3.1**
  (127 overwritten files backed up to `local/backups/skse-2.2.6/`). Installed **Address Library v13 All in One** (contains `versionlib-1-7-104-0.bin`).
  Every change is listed in `local/install-manifest.json`.
- **Found:** me3 profiles can't store a save file, so launches must pass `--savefile skyrimxer.sl2`. me3 blocks matchmaking by default.
  MO2 is installed but has no instance, so files went straight into `Data\`.
- **Tested:** File versions checked (`skse64_1_7_104.dll` / loader = 2.3.1). `me3 info` → installation Found. **Not yet tested in-game.**
- **In-game check (same day):** Launched through `skse64_loader.exe`. `skse64.log`: SKSE 2.3.1 initialized, CrashLogger "loaded correctly",
  kDataLoaded reached. ✔ The Skyrim side of the toolchain works.
- **Next:** me3 smoke test with Elden Ring (no natives, `--savefile skyrimxer.sl2`), then the rest of Phase 0.

## 2026-10-03: Project scaffold
- **Changed:** Created the repo structure, local agent-rules file, design/roadmap/recon docs, whitelist `.gitignore`, me3 profile template,
  and folder READMEs. Ran `git init`. Cloned the reference repos into `reference/` (ignored).
- **Environment found:** Skyrim `SkyrimSE.exe` 1.7.104.0 + SKSE 2.2.6 (no Address Library yet). Elden Ring `eldenring.exe` 2.7.1.0.
  VS 2026 C++ ✔, Rust ✔, git ✔. Missing: me3, Address Library.
- **Decision:** Skyrim plugin uses SkyCraft's CMake + `alandtse/CommonLibVR` (ng) submodule setup, so xmake isn't needed. Fake-peer test processes are planned for P2.
- **Tested:** `git status --ignored` confirms only docs/config are tracked and game/build/local folders are ignored.
- **Next:** Phase 0 (checking tools & versions). See `STATUS.md`.
