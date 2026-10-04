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
