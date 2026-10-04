# Elden Ring player state (what PlayerState reads)

Game/version: eldenring.exe 2.7.1.0 (1.17.1), eldenring-rs `59fbd3b`. Read in `er-plugin/src/game.rs` (`snapshot`), published by
`er-plugin/src/remote.rs` (`publish_state`).

## Fields
- HP/FP/stamina + maxes: `modules.data` (`CSChrDataModule`). Anim: `modules.time_act.anim_queue[read_idx].anim_id` (backstep = 27010;
  spawn-in plays 63000 / 29030000 first, during which a backstep press does nothing). Anim ids carry an animation-group prefix
  (millions): with another stance/equipment the backstep is 12027010 and idle 12000000, so match `anim % 1_000_000 == 27010`. Poise: `modules.super_armor`. Transform:
  `modules.physics`. Map: `PlayerIns.current_block_id`.
- Publish timing: ChrIns_PostPhysics while the player exists. The ChrIns task groups barely run at the title screen, so `FrameBegin`
  publishes "not in world" (flags 0) there; otherwise Skyrim would see PlayerState go stale.

## Stamina: dodges are free out of combat
- Out of combat, stamina never drops on a backstep (real and injected, many runs). With an enemy aggroed (m60_42_37_00), a backstep costs
  **8** (136→128) and a roll **12** (136→124), then it regenerates in ~0.5 s.
- Confidence: verified by test (2026-10-04).

## Combat flag: `CSChrDataModule` byte +0x19a, bit 0x40 (set = out of combat)
- eldenring-rs maps these bytes as `unk198` (the u32 at +0x198 reads 0x20400050 calm, 0x20000050 in combat). `er-plugin/src/combat.rs`.
- **Found by:** a raw memory dump every 15 frames (PlayerIns, PlayerGameData, 10 typed modules) while the user rolled calm, then near an
  aggroed enemy, then after resting at a grace. Of all words, this bit alone separated the 7 free rolls from the 8 costly ones with only
  4 changes: cleared 15 frames before the first costly roll (enemy aggro), set again while sitting at the grace.
- **Not a SpEffect:** no active effect has `consume_stamina_rate` ≠ 1, and combat began with no SpEffect change (effects 26/4202 don't match).
- **Recomputed every frame in ChrIns_NaviCache:** written in every task group from WorldChrMan_Prepare to ChrIns_PostPhysics, only
  NaviCache found it changed back (300/300 frames). A write in ChrIns_AILogic holds through behavior.
- **Forcing works:** with the bit cleared every frame and no enemy near, 5/5 injected rolls cost 12 stamina (136→124, then regen).
- Confidence: verified by test (2026-10-04, ER self-test `-ErSelfTest roll -ErForceCombat on`).

## I-frame flag: `action_modifiers_flags` bit 1 (FLAG_AS_DODGING), rolls only
- **Rolls set it, backsteps don't.** `CSChrActionFlagModule.action_modifiers_flags` (u64, module +0x40) bit 1 (eldenring-rs `dodging`,
  TAE event 0 action 8) is set for **26–27 frames at 60 fps from the roll animation's first frame** (anim 27110, ~0.45 s ≈ 13 frames at
  30 fps), on every one of ~20 rolls. The spawn-in animation (63000) also sets it, while the player is invulnerable.
- **Hit timing confirms it (2026-10-04, user play test, enemy in m60_42_37_00):** 46 HP losses logged with the modifier bits and the frame
  offset into the last dodge. **None landed while bit 1 was set.** Hits taken mid-roll came at +30 and +36 (just after the window).
- **Backsteps (27010 / 12027010) set no invincibility bit**, and hits landed at +1, +7, +8, +18 and +34 frames into them: in this setup a
  backstep has no usable i-frames. So the i-frame window needs a directional roll (P3 step 5).
- `PlayerFlag::IFrame` = bit 1 or bit 0 (perfect_invincibility) or bit 3 (invincible excluding throws) or bit 5 (PvE-only i-frames)
  (`er-plugin/src/game.rs`). Bits 0/3/5 were never seen set during dodges; they're kept because eldenring-rs maps them as invincibility.
- **Frame phase:** the u64 is rebuilt every frame. It reads 0 during the HavokBehavior task group and has its new value from
  ChrIns_PrePhysics onward, so the PostPhysics read (PlayerState publisher) sees the current frame's value. Other bits seen: 15 = whole
  roll/backstep (`disable_turning`), 32 at the end of a roll, 42 while running (anim 20110).
- **Ruled out earlier (attempts 1–2, backsteps only, so they couldn't have shown it):** `CSChrEventModule.flags` reads a constant 0xff;
  `ChrIns.chr_flags1c4..1cb` bits flip irregularly (render/visibility-like). **Special effects:** no effect is added during a backstep.
  A roll adds 100240 (+0..+6), 100390 (+5..+8) and 430 (+0..+33), all shorter or longer than the i-frame window, so they aren't it.
- Confidence: verified by test (rolls + hit timing, 2026-10-04). Exact roll weights (light/heavy) not measured yet.

## Movement-cancel window and moving the character (P4 step 4)
- `CSChrActionRequestModule.tae_cancels` bit 1 (`movement_cancel`, TAE CANCEL_LS_MOVEMENT) or bit 2 (previous frame) = movement may end
  the current animation. In a roll (27110) it opens ~43 frames in (the animation runs ~99). Published as PlayerFlag MoveCancel.
- Rolls chained by spamming keep anim id 27110 (each opens a new dodge-flag window); chained backsteps keep 27010 with no flag.
- A Sprint hold past the tap window dashes (12020110/12020210); with the move stick dropped after 20 frames the character stands.
- Teleport: write `CSChrPhysicsModule.position` and set `chr_proxy_pos_update_requested` (ChrIns_PostPhysics) → the character is there
  next frame (`er-plugin/src/park.rs`). Confidence: verified by test (2026-10-04).
- **Pinning (P4 step 4b):** the same write *every frame* in ChrIns_PostPhysics (back to the spot's X/Z, Y left to ER) makes the next
  physics step start from the spot again: the per-frame displacement is the full root-motion step (roll 27110 = 3.51 m total, the same
  pinned and free, all four directions) and the character never gets further than ~0.17 m from its spot. I-frame window (27 frames),
  anim ids and roll timing unchanged. So no root-motion read is needed: displacement after physics minus the spot = this frame's step.
  Confidence: verified by test (2026-10-04, A/B self-test `-ErSelfTest roll` with/without `-ErNoPin`, then both games).
