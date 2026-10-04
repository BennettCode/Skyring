# STATUS

_Last updated: 2026-10-04_

## Current phase
**P3: in progress** (plan: `docs/P3-PLAN.md`). Step 1 done: ER runs hidden in-world at 60 fps (focus spoof).
Step 2: the injected backstep **works while the ER window is focused** (virtual `Backstep` hold + `BackstepTapped` on the press
frame). Open: the hidden/unfocused case (see Handoff notes).
Both plugins share the region `Local\SkyrimXER_v1`, handshake, exchange heartbeats, and fail safe when the other game dies or quits.

## What exists
- `protocol/`: schema `schema/messages.toml` (v1) → `tools/protogen` → `generated/skyrimxer_protocol.{h,rs}`. Rust crate `skyrimxer-protocol`
  (region, rings, link state machine). Layout + link rules: `docs/DESIGN.md` §4.
- `skse/` → `SkyrimXER.dll` (CommonLibVR-ng 39f9d07, AE only). Logs load/kDataLoaded/save loaded. Link thread starts on kDataLoaded
  (`src/bridge/`), Bye on clean exit. Also builds `skyrimxer_link_test.exe` (C++ selftest + peer mode).
- `er-plugin/` → `skyrimxer_er.dll` (eldenring-rs 59fbd3b). Version check, FrameBegin task (frames, focus spoof, hides
  the window in-world, player state every 5 s), link thread. Dev switches in `build/er-plugin/skyrimxer_er.cfg` (written by `dev.ps1`).
- `tools/`: dev.ps1 (one-command loop), setup-check, build, deploy (+Undo/WhatIf), backup-saves, launch, collect-logs (prints [core]/[link]),
  protogen, fake-peer. `tests/run-tests.ps1`: all protocol/link tests, no game needed (≈ 20 s).
- Installed in the game (see `local/install-manifest.json` + `local/deploy-manifest.json`): SKSE 2.3.1, Address Library v13, Crash Logger, SkyrimXER.dll.

## Blockers / open questions
- **Hidden-window input** (P3 step 2, last part), see Handoff notes. Arena settled: the test character is parked in m10_01_00_00 and the user presses
  Continue each run (no warp in P3). The user tests with a DualSense (PS5) over USB in both games.

## Next 3 steps (P3, full plan in `docs/P3-PLAN.md`, approved 2026-10-04)
1. ~~ER keeps running hidden~~ ✔ (60 fps hidden, `er-plugin/src/window.rs`).
2. ~~Injected dodge, window focused~~ ✔. Left: make the action module read the pad while ER is hidden (find the remaining focus check).
3. Protocol v2 (InputState + PlayerState seqlock slots), then wire Skyrim Sprint → ER dodge → PlayerState back to Skyrim.

## Handoff notes
**P3 step 2 (2026-10-04, session 3).** The injected backstep works while ER is the foreground window: hold `Backstep` (virtual 270) and
raise `BackstepTapped` (virtual 269) on the press frame only, like a real tap. When ER is not foreground (hidden, loading, or another
window focused), the pad poll still sees the keys but `CSChrActionRequestModule.action_requests` stays 0. So one more focus check sits
between the pad and the player's manipulator. Next: compare the typed focus/window fields focused vs unfocused and spoof the one that differs.
Details: `docs/research/elden-ring-input.md` ("session 3").
