# STATUS

_Last updated: 2026-10-04_

## Current phase
**P3: in progress** (plan: `docs/P3-PLAN.md`). Step 1 done: ER runs hidden in-world at 60 fps (focus spoof).
Step 2 done: the hidden ER player backsteps from code (virtual `Backstep` hold + `BackstepTapped` on the press frame; focus spoof
also after PadStep). Step 3 done: protocol v2 adds the seqlock slots `InputState` (sky→er) and `PlayerState` (er→sky), tested without the games.
Both plugins share the region `Local\SkyrimXER_v2`, handshake, exchange heartbeats, and fail safe when the other game dies or quits.

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
- None blocking. Arena settled: the test character is parked in m10_01_00_00 and the user presses
  Continue each run (no warp in P3). The user tests with a DualSense (PS5) over USB in both games.

## Next 3 steps (P3, full plan in `docs/P3-PLAN.md`, approved 2026-10-04)
1. ~~ER keeps running hidden~~ ✔ (60 fps hidden, `er-plugin/src/window.rs`).
2. ~~Injected dodge in hidden ER~~ ✔ (`er-plugin/src/actions.rs`, `pad.rs`, `window.rs`).
3. ~~Protocol v2 (InputState + PlayerState seqlock slots)~~ ✔ (`protocol/src/slot.rs`, `skse/src/bridge/Slot.h`).
4. Wire the loop: Skyrim Sprint → InputState → ER injector → PlayerState back to Skyrim (`docs/P3-PLAN.md` step 4; new engine hook +
   Address Library check, so plan mode).

## Handoff notes
**P3 step 3 done (2026-10-04).** Protocol v2 slots are in, and the tests/fake peers exercise them. The plugins were only rebuilt against v2
(no behaviour change). Step 4 hands `Link::shared()` / `Link::Shared()` to the game threads and moves the peers' `PlayerWatch` edge
logger into the Skyrim plugin.

**P3 step 2 done (2026-10-04, session 3).** Hidden ER backsteps from code: hold `Backstep` (virtual 270) + `BackstepTapped` (virtual 269)
on the press frame only, written at WorldChrMan_Prepare, with the focus spoof run at FrameBegin and again at WorldChrMan_Prepare.
Open note: the i-frame/dodging flags read 0 during backsteps (real ones too); check before PlayerState uses them.
Details: `docs/research/elden-ring-input.md` ("session 3").
