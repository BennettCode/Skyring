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
  **8** (136→128), then it regenerates in ~0.5 s.
- Confidence: verified by test (2026-10-04).

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
