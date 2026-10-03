# Elden Ring: input injection and focus (Phase 0 findings)

- Game/version: eldenring.exe 2.7.1.0 WW (patch 1.17.1.0)
- Source: eldenring-rs @ 59fbd3b (2026-09-20), read in `reference/eldenring-rs` (notes only, no code copied)
- Confidence: **read from the bindings, not yet tested in-game.** Verify in P3.

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
