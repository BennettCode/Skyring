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

## I-frame flag: not found yet
- `CSChrActionFlagModule.action_modifiers_flags` (u64 at module +0x40): eldenring-rs bit 0 `perfect_invincibility` and bit 1 `dodging` stay 0
  for the whole backstep.
- Word-diff probe (`actions.rs`, every frame of the dodge watch window, all u32 words of the module), same pattern on every backstep:
  - +0x010 (`animation_action_flags`, bit 0 `stay_state`): 1 → 0 when anim 27010 starts (+9 frames after the press), back to 1 at +89.
  - +0x040 bit 15 (eldenring-rs: `disable_turning`) is set from +12 to +90 frames. That's the whole animation, too long for i-frames.
  - +0x1d8: 0 → 1 for exactly one frame at the end (+89).
- Same result **in combat** (enemy attacking): only those markers, plus +0x010 bit 4 (set while in combat: the word reads 0x11 instead of 0x1).
- So nothing in this module looks like an i-frame window. Next places to look: the same probe with an enemy attacking (i-frames may only
  matter, or only be set, in combat), or other modules (special effects, `ChrIns` flags).
- Confidence: observed (2026-10-04, 8 backsteps).
