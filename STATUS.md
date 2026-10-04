# STATUS

_Last updated: 2026-10-04_

## Current phase
**P2: done** (2026-10-04, verified in both games). **Next: P3 (one value across, then the loop).**
Both plugins share the region `Local\SkyrimXER_v1`, handshake, exchange heartbeats, and fail safe when the other game dies or quits.

## What exists
- `protocol/`: schema `schema/messages.toml` (v1) → `tools/protogen` → `generated/skyrimxer_protocol.{h,rs}`. Rust crate `skyrimxer-protocol`
  (region, rings, link state machine). Layout + link rules: `docs/DESIGN.md` §4.
- `skse/` → `SkyrimXER.dll` (CommonLibVR-ng 39f9d07, AE only). Logs load/kDataLoaded/save loaded. Link thread starts on kDataLoaded
  (`src/bridge/`), Bye on clean exit. Also builds `skyrimxer_link_test.exe` (C++ selftest + peer mode).
- `er-plugin/` → `skyrimxer_er.dll` (eldenring-rs 59fbd3b). Version check, FrameBegin task (counts frames), link thread after the version check.
- `tools/`: dev.ps1 (one-command loop), setup-check, build, deploy (+Undo/WhatIf), backup-saves, launch, collect-logs (prints [core]/[link]),
  protogen, fake-peer. `tests/run-tests.ps1`: all protocol/link tests, no game needed (≈ 20 s).
- Installed in the game (see `local/install-manifest.json` + `local/deploy-manifest.json`): SKSE 2.3.1, Address Library v13, Crash Logger, SkyrimXER.dll.

## Blockers / open questions
- None for starting P3. P3 open questions: arena location, hiding ER's window + focus spoof (ER ran ~60 fps unfocused at the title
  screen in the P2 test, still to be checked in-world), input injection path (`CSChrActionRequestModule` bits), coordinate test.

## Next 3 steps (P3)
1. Plan P3 (input slot + PlayerState slot = protocol v2, focus/hide on the ER side, arena warp) and write it here.
2. ER: hide the window + focus spoof; warp the test character to the arena. Prove ER keeps simulating while hidden (frames + player state in logs).
3. Skyrim → ER: forward the dodge button; ER rolls; ER logs stamina before/after. Then PlayerState back to Skyrim at 1-in-30 frames.

## Handoff notes
(When a session gets stuck: goal, what was tried + why it failed, current hypothesis, key files/log lines, 3 alternative approaches.)
