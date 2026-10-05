# Elden Ring: input injection and focus (Phase 0 findings)

- Game/version: eldenring.exe 2.7.1.0 WW (patch 1.17.1.0)
- Source: eldenring-rs @ 59fbd3b (2026-09-20), read in `reference/eldenring-rs` (notes only, no code copied)
- Confidence: Phase 0 sections = read from the bindings. **"P3 in-game results" below = tested 2026-10-04.**

## Action injection, no OS keystrokes (planned approach)
- `eldenring::cs::chr_ins::module::action_request::CSChrActionRequestModule` lives on each `ChrIns`.
  It's updated during `CSTaskGroupIndex::ChrIns_PreBehavior`.
  - `action_requests: ChrActions` = raw action inputs "from the pad manipulator" for this frame.
  - The engine derives `new_action_presses` / `released_actions` by comparing with `previous_action_requests`, so a "press" means
    set the bit for at least one frame and then clear it. Holding (charged R2, guard) means keeping the bit set. `action_timers` tracks hold length.
  - `possible_action_inputs` / `possible_action_cancels` / `disabled_action_inputs` = the engine's own gating (recovery frames, stamina, etc.).
    **Don't override these.** That gating is the ER behaviour we're importing.
- `ChrActions` (u64 bitfield) has the bits we need (bit → meaning):
  `0 r1` light attack · `1 r2` heavy · `2 l1` off-hand/guard-attack · `3 l2` skill/heavy-off-hand · `5 sp_move` dodge/roll/backstep ·
  `6 jump` · `7 use_item` (flask) · `12 r3` lock-on · `13 l3` crouch · `17 rolling` · `16 backstep` · `24 guard` · `25 emergencystep` ·
  `26/27 light/heavy kick` · `28/29 change_style_r/l` (two-hand) · `19/20/33/34 magic`.
- **Open question (P3):** where does *movement direction* (left stick) go? Candidates: `movement_request_duration` / `movement_request_flags`
  on the same module, the pad manipulator (`cs/pad.rs`), or `dluid` virtual devices. The roll direction depends on it.

## Focus / hidden window
- `eldenring::dluid::user_input_manager::DLUserInputManagerImpl` has `window_handle: isize` and **`is_game_window_focused: bool`**.
  Plan: hide the window and force `is_game_window_focused = true` every frame (SkyCraft's "hidden but told it's focused").
  Because we inject actions directly (above), we may not need OS-level input focus at all. Still needed so the game doesn't stop reading pad state.
- Community reports: ER handles alt-tab badly in exclusive fullscreen and can stutter afterwards. **Run ER windowed (not exclusive fullscreen) at a
  low resolution** for the hidden instance. If keys are held during a focus change, clear the input state.

## Reading state back (for PlayerState)
- Pattern from `examples/apply-speffect`: `WorldChrMan::instance_mut()` → `.main_player` (a `PlayerIns`) → `.chr_ins`.
  Next: find HP/FP/stamina, poise, current animation, and i-frame/hyperarmor flags in `cs/chr_ins/**` and `cs/player_game_data.rs` (P3).

## P3 in-game results (2026-10-04, test character idle in m10_01_00_00)

**Hidden + focus spoof works.** Each FrameBegin: `DLUserInputManagerImpl.is_game_window_focused = true`,
`FD4PadManager.is_back_ground_window = false` / `exit_foreground_signaled = false`. Then `ShowWindow(CSWindowImp.window_handle, SW_HIDE)`
once the main player exists. ER keeps running at **60 fps** while hidden, with another window in the foreground (`er-plugin/src/window.rs`).

**Frame order of our tasks** (probe log order): PadStep → WorldChrMan_Prepare → ChrIns_AILogic → ChrIns_PreBehavior → ChrIns_PreBehaviorSafe
→ HavokBehavior → ChrIns_PrePhysics → ChrIns_CalcUpdateInfo (yes, after PrePhysics) → ChrIns_PostPhysics.

**A real Space/Circle press** (window visible + focused), sp_move bits per group (R=action_requests, N=new_action_presses,
P=possible_action_inputs, r=previous_action_requests):
- The engine's `CSChrActionRequestModule` update runs inside ChrIns_PreBehavior, after our task in that group: R/N flip between the PreBehavior
  probe and the PreBehaviorSafe probe. The press frame shows R1 N1. While held, R1 stays on; N is only 1 on the first frame.
- `possible_action_inputs.sp_move` was 1 around the real press (Pad..PreBehavior and PrePhysics..PostPhysics) but 0 at PreBehaviorSafe/Havok.
  It was 0 in every frame of our injected attempts. Meaning not yet understood.
- Anim stays 202100 while the button is held, and the dodge anim (27010 = backstep) starts on **release**. That's ER's tap=dodge/hold=dash rule.

**Attempt 1: write `action_requests.set_sp_move(true)` in ChrIns_PreBehavior. FAILS.** The engine rebuilds `action_requests` from its input
source in its PreBehavior update every frame. Our bit shows up afterwards (wrong order), or gets overwritten (frame after). `new_action_presses` never goes to 1.

**Attempt 2: virtual pad. Hold `UserInputKey::Backstep` on the VirtualMultiDevice. FAILS so far.** The key-assign lookup returns one slot:
mapped input **783 → virtual index 270**, checked=true. We write it in WorldChrMan_Prepare (`er-plugin/src/pad.rs`). The write persists
through the next PadStep, and `CSInGamePad.poll_digital_input(Backstep)` reads **1** in every group while held. But `action_requests.sp_move`
stays 0, so the player's pad manipulator does not read the CSInGamePad poll (or ignores it while the window is hidden).

Open hypotheses (next session):
1. The manipulator ignores input while the window is **not really foreground** (we only spoof two flags). Test: the same self-test with
   `-ErVisible` and the window clicked/focused, hands off. If it works there, find the remaining focus check.
2. The manipulator reads a **different source** than CSInGamePad (e.g. the pad device / libScePad directly, a per-device `PadDevice` state,
   `unused_input_map`, or a manipulator-side cache filled before WorldChrMan_Prepare). Test: write at PadStep (`-ErInjectGroup padstep`), and also
   write `initial_virtual_input_data` before the copy. Mapped input 783 may be the *keyboard* binding; the user plays with a **DualSense (PS5) over USB**,
   so try the pad-mapped input / `PadDevice` (`s_thumb*`, `w_buttons`).
3. Hold/release timing: hold longer (10+ frames) and check `BackstepTapped` (13) too.

## P3 step 2, session 2 results (2026-10-04, window visible + focused, test character idle in m10_01_00_00)
- **Focus matters for the action module.** With `-ErVisible` and the window clicked, the virtual Backstep hold reaches
  `CSChrActionRequestModule`: `action_requests.sp_move` 1 while held, `new_action_presses` 1 on the first frame, `action_timers.roll` counts
  up 1/60 s per frame, release clears it. Hidden (focus spoof only) it stays 0, so the spoof is still missing a check.
- **Still no dodge**, for holds of 3, 4, 8, 12, 20 and 30 frames: no anim change, no movement, no stamina change. So hold length isn't the cause.
- **Gating masks at idle (end of frame), same for real and injected presses:** `possible_action_inputs` = 0x0, `possible_action_cancels` =
  0x73c1ffeef (includes sp_move), `disabled_action_inputs` = 0x0, `queued_action_inputs` = 0x0, `animation_action_flags` = 0x1 (stay_state).
  During the backstep anim 27010: possible = cancels = 0x73d1ffeef, flags 0x0. So `possible = 0` at idle is normal and not the blocker.
- **Same-session real Circle tap** (DualSense): R held about 9 frames, P0 Q0 throughout. Anim 27010 began **while still held**, one frame before
  the pad poll saw the release. Every module field we log looks the same for the real tap and for ours. So whatever starts the dodge
  (behavior script) reads input from somewhere other than `CSChrActionRequestModule` and `CSInGamePad`.
- **Next candidates:** (1) the player's pad manipulator (`PlayerIns.chr_manipulator`, `ManipulatorType::Pad`, untyped in eldenring-rs):
  compare its bytes across a real tap and an injected one with a bounded memory-diff probe. (2) Mapped input 783 is probably the keyboard
  binding: inject the pad-mapped Circle input and `UserInputKey::BackstepTapped` instead. (3) Skip input: ask the behavior for the
  evasion directly, then check that ER's stamina gating still applies.

## P3 step 2, session 3 results (2026-10-04): injected backstep WORKS (window focused)
- **Key layout** (`pad::describe`): `Backstep` (12) = mapped 783 → virtual index 270 (AreKeysDown). **`BackstepTapped` (13) = its own
  mapped input 784 → virtual index 269** (AreKeysDown). Movement: `MovementControl` 759 → 273; `MoveForwards/Backwards/Left/Right` =
  760..763 → 66/65/64/63 (IsStickMoving, analog).
- **Real DualSense ◯ tap, per frame:** press frame: polls Backstep **and** BackstepTapped (BackstepTapped only on that one frame), merged
  device live+initial bit set; `action_requests` = sp_move (0x20) while held, `roll` timer counts. **Release frame: `action_requests` = 0x10000
  (bit 16 `backstep`, new press), sp_move released**; anim 27010 the next frame. `queue_mode_enabled` = 1, queue empty at idle.
- **Ours before:** identical except BackstepTapped never polled → on release only sp_move was released, no bit 16, no backstep.
- **Fix:** hold `Backstep` as before and set `BackstepTapped`'s slot for the **press frame only**. Result: release → bit 16 → anim 27010,
  for 9- and 30-frame holds (30-frame: backstep starts ~9 frames in while still held, like a real tap). No gating masks touched.
- **Remaining gate = focus:** in the same run some pulses had the pad poll at 1 (K1 T1) but `action_requests` stayed 0 for the whole press.
  That happened during load-in and while the user was typing in another window. So `CSChrActionRequestModule`'s read from the pad needs
  the window to really be foreground; our two-flag spoof (`is_game_window_focused`, `is_back_ground_window`) isn't enough. Next: find that check (P3 step 2, Stage 3).
- Stamina stayed 101 for both the real and the injected backstep (out of combat, idle); not a difference.

## P3 step 5: move stick from code (2026-10-04)
- The movement keys are analog slots on the same VirtualMultiDevice: `MoveForwards` → 66, `MoveBackwards` → 65, `MoveLeft` → 64,
  `MoveRight` → 63 (`set_virtual_analog_state`, written in WorldChrMan_Prepare every frame while held, like the digital keys).
- **Sign:** Forwards/Right take a positive value (+1 = full), **Backwards/Left take a negative value** (−1 = full). A positive value on
  Backwards/Left is ignored (no walking, the dodge stays a backstep). `CSPad::poll_analog_input` reads back exactly what was written.
- Stick held + Backstep tap (4 or 9 frames, BackstepTapped on the press frame) → roll anim 27110 with the dodge flag for 27 frames, in all
  four directions (hidden window). A 30-frame hold with a direction = dash (12020210 → 12022200), ER's normal hold behaviour.
- The roll direction is relative to ER's camera; mapping it to Skyrim's world is the coordinate test.

## P3 step 2, session 3: hidden window solved (2026-10-04)
- **Focus probe** (`er-plugin/src/focus.rs`, change-only, FrameBegin before our spoof + WorldChrMan_Prepare): with ER not foreground,
  `FD4PadManager.is_back_ground_window` is **set to 1 again during PadStep every frame**; our FrameBegin spoof was too early.
  `DLUserInputManagerImpl.is_game_window_focused` only flickers to 0 for a frame. Unnamed neighbours (pad manager unk10/unk2fa,
  input manager unk889/88a/88c, foreground cooperative-level flags) don't change with focus. Pad entries `enable_use`/`allow_polling` churn with menus, not focus.
- **Fix:** run the same spoof again in a WorldChrMan_Prepare task (after PadStep, before ChrIns_PreBehavior reads input).
- **Result, window hidden, hands off:** 7/8 self-test pulses backstep (anim 27010; 4-, 9- and 30-frame holds), 60 fps. The miss was the
  pulse sent while the character was still spawning (anim -1), as in every earlier run.
- **Open (not blocking):** `perfect_invincibility` / `dodging` flags stayed 0 during 27010 for real and injected backsteps alike, and stamina
  stayed 101 (idle, out of combat). Confirm where ER exposes backstep/roll i-frames before PlayerState relies on them (P3 step 3/4).

## Weapon stance from code (2026-10-05)
- ChrAsm (eldenring-rs `player_game_data.rs`) exists twice: live (`PlayerIns.chr_asm`) and saved (`PlayerGameData.equipment.chr_asm`).
  Writing `equipment.arm_style` and `selected_slots` in both switches ER's animation set on the next action. ER backstep self-test,
  test character (R1 colossal sword 4000124, R2 dagger 1000700, L1 shield 31140000, L3/R3 Unarmed 110000):
  - RightBothHands (the save's stance): 12027010.
  - OneHanded or EmptyHanded with slot 1: 2027010 (the sword is still in hand).
  - OneHanded with slots L3/R3 (Unarmed): **27010**, the plain set.
- `er-plugin/src/stance.rs` (StanceSync) follows InputState.stance and puts the save's stance back when the bridge lets go.

## The physical pad and ER buttons from Skyrim (P4 step 8, 2026-10-05)
- **ER reads the DualSense itself** while hidden and focus-spoofed: its `PadDevice` (user 0) showed the sticks moving with Skyrim focused,
  and R1 on the pad played ER attack anims (20030000...) with nothing injected. So the pad pressed both games.
- Fix: while bridged, after PadStep (`WorldChrMan_Prepare`, before our writes), every action `UserInputKey` is released through the
  key-assign lookup (`pad::set_digital(key, false)`: 21 slots) and every value of the virtual device's analog vector is zeroed (813
  values, camera included). Then Skyrim's inputs are written.
- **Don't bulk-clear `DynamicBitset`:** for 813 analog values its `integer_count` was 124. eldenring-rs treats it as u32 words (3968
  bits), but its own comment ("bit_count // 32 * 4") reads as bytes; a `fill(0)` over 124 words would write past the allocation.
- Held virtual keys from Skyrim (fake-peer `--buttons`, character idle, two-handed stance): Attack → 42030000, StrongAttack (250 ms) →
  42030500 → 42030505 (charge), Guard → 42034000, Skill → 712040000.

## Locomotion probe (LOCO-PLAN stage B1, 2026-10-05)
ER only, hidden window, character pinned (park.rs), `tools/dev.ps1 -ErSelfTest walk|sprint` (`actions::LocoTest`: a fixed script of
virtual stick values and Dodge holds, logged every 10 frames as `[loco-test]`).
- **Camera yaw:** `CSCamera.pers_cam_1.matrix` row 2 = forward; `yaw = atan2(-f.x, -f.z)` is in the player's yaw convention
  (`game::camera_yaw`). At spawn, player yaw = camera yaw (−1.756). Stick right → player yaw = camera + 90°, back → camera + 180°:
  the stick is camera-relative and right = +angle, like the player's yaw. The hidden camera **does not turn** while the character walks
  or runs (no auto-follow), so the stick-to-world mapping only changes when we turn it.
- **Turning:** the character faces the stick direction within 10 frames (ER's own turn, no extra smoothing needed).
- **Speeds** (flat ground, stick magnitude): 0.3 = nothing (dead zone); 0.6 = walk, anim 20010, ~1.5 m/s; 1.0 = run, anim 20110,
  3.93 m/s steady. Stop anim 22100. Same speed in every direction (forward, right, back).
- **Sprint:** stick + Dodge held → run, then sprint (anim 20210) after ~30 frames, 5.2–6.7 m/s per 10-frame sample (mean 5.95),
  stamina −10/s with combat forced; no dash at the start while already running. Release → back to run, stamina regenerates.
- **Roll from a run:** Dodge tap (press frame + 6 frames) while running → roll 27110 (−12 stamina in combat), then back to the run
  (20110) by itself, with the stick still held.
- **Pin:** the real position stayed on the spot the whole time (1.67, 3.49); the virtual position carried all the distance.
