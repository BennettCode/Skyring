# STATUS

_Last updated: 2026-10-04_

## Current phase
**P1: done** (2026-10-04). **Next: P2 (shared memory + heartbeat).**
Both plugins load in their games at the same time and log in the shared format. All dev scripts exist and work.

## What exists
- `skse/` → `SkyrimXER.dll` (CommonLibVR-ng submodule pinned to 39f9d07 / v10.1.0, AE only). Logs load + kDataLoaded/new game/save loaded.
- `er-plugin/` → `skyrimxer_er.dll` (eldenring-rs pinned to 59fbd3b). Logs load, version check, FrameBegin task, main player spawned/gone.
- `tools/`: setup-check, build, deploy (+Undo/WhatIf), backup-saves, launch, collect-logs, common.psm1.
- Installed in the game (see `local/install-manifest.json` + `local/deploy-manifest.json`): SKSE 2.3.1, Address Library v13, Crash Logger, SkyrimXER.dll.
- **Nothing is committed to git yet** (the user hasn't asked). Everything is staged/untracked and the whitelist has been checked.

## Blockers / open questions
- None for P2. P3 still has open questions: arena location, movement-direction source, both-games performance, hiding the ER window.

## Next 3 steps (P2)
1. `protocol/schema/messages.toml` v1 (header + Hello/Bye/Heartbeat) + `tools/protogen` (Rust) → generated `.h` + `.rs` with size/offset asserts.
2. Shared region `Local\SkyrimXER_v1` on both sides: open-or-create, magic/version check, 1 Hz heartbeats, 2 s timeout fail-safe.
3. `fake-skyrim` / `fake-er` stand-in processes so each side can be tested without the other game. Then the kill-either-game test.

## Handoff notes
(When a session gets stuck: goal, what was tried + why it failed, current hypothesis, key files/log lines, 3 alternative approaches.)
