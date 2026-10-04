# STATUS

_Last updated: 2026-10-04_

## Current phase
**P4: in progress** (plan: `docs/P4-PLAN.md`). Step 1 done (2026-10-04): while the bridge is on, Skyrim's vanilla sprint is off (Sprint =
ER dodge), F10 toggles the bridge, and the hidden ER character only gets the move stick around a dodge (stays parked).
Step 2 done: ER's combat flag found (`CSChrDataModule` +0x19a bit 0x40) and forced from code (rolls then cost stamina with no enemy).
P3 (done): ER runs hidden at 60 fps; Sprint (+ W/A/S/D) → ER backsteps/rolls by ER's rules; stamina, animation and the roll i-frame
window come back every frame; coordinate conversion measured (`protocol/src/coords.rs`). Region `Local\SkyrimXER_v2`.

## What exists
- `protocol/`: schema `schema/messages.toml` (v2) → `tools/protogen` → `generated/skyrimxer_protocol.{h,rs}`. Rust crate `skyrimxer-protocol`
  (region, rings, seqlock slots, link state machine + `LinkShared` for game threads). C++ mirrors: `skse/src/bridge/{Link.*,Slot.h}`.
  Layout + link rules: `docs/DESIGN.md` §4.
- `skse/` → `SkyrimXER.dll` (CommonLibVR-ng 39f9d07, AE only). Logs load/kDataLoaded/save loaded. Link thread starts on kDataLoaded
  (`src/bridge/`), Bye on clean exit. Also builds `skyrimxer_link_test.exe` (C++ selftest + peer mode).
- `er-plugin/` → `skyrimxer_er.dll` (eldenring-rs 59fbd3b). Version check, FrameBegin task (frames, focus spoof, hides
  the window in-world, player state every 5 s), link thread. Dev switches in `build/er-plugin/skyrimxer_er.cfg` (written by `dev.ps1`).
- `tools/`: dev.ps1 (one-command loop), setup-check, build, deploy (+Undo/WhatIf), backup-saves, launch, collect-logs (prints [core]/[link]),
  protogen, fake-peer (`skyrim` pulses Dodge, `er` fakes a dodge into PlayerState). `tests/run-tests.ps1`: all protocol/link/slot tests,
  no game needed (≈ 25 s).
- Installed in the game (see `local/install-manifest.json` + `local/deploy-manifest.json`): SKSE 2.3.1, Address Library v13, Crash Logger, SkyrimXER.dll.

## Blockers / open questions
- **Skyrim + DualSense:** Skyrim sees no gamepad when started by skse64_loader (no Steam Input). Keyboard Sprint (Left Shift) is used for
  P3; controller support comes later (`docs/research/skyrim-hooks.md`).
- None blocking. Arena settled: the test character is parked in m10_01_00_00 and the user presses
  Continue each run (no warp in P3). The user tests with a DualSense (PS5) over USB in both games.

## Next 3 steps (P4, `docs/P4-PLAN.md`)
1. Step 3: protocol v3 (InputState `flags`: InCombat, BridgeOn) so Skyrim's combat state drives ER's (`combat::set_in_combat`).
2. Step 4: the Skyrim player follows ER's roll (ER displacement → coords → Skyrim, with Skyrim collision).
3. Step 5: vanilla Silent Roll animation.

## Handoff notes
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
