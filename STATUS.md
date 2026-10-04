# STATUS

_Last updated: 2026-10-04_

## Current phase
**P3: in progress** (plan: `docs/P3-PLAN.md`). Step 1 done: ER runs hidden in-world at 60 fps (focus spoof).
**Blocked on step 2:** injecting a dodge into ER from code (two approaches failed; see Handoff notes).
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
- **Dodge injection into ER** (P3 step 2), see Handoff notes. Arena settled: the test character is parked in m10_01_00_00 and the user presses
  Continue each run (no warp in P3). The user tests with a DualSense (PS5) over USB in both games.

## Next 3 steps (P3, full plan in `docs/P3-PLAN.md`, approved 2026-10-04)
1. ~~ER keeps running hidden~~ ✔ (60 fps hidden, `er-plugin/src/window.rs`).
2. ER self-test: inject `sp_move` (dodge) via `CSChrActionRequestModule`; log stamina/anim/i-frame flags.
3. Protocol v2 (InputState + PlayerState seqlock slots), then wire Skyrim Sprint → ER dodge → PlayerState back to Skyrim.

## Handoff notes
**P3 step 2, dodge injection (stuck, session 2, 2026-10-04).** Goal: the hidden ER player dodges from code, with ER's own gating intact.
Session 2: with the window **visible + focused**, the virtual Backstep hold reaches `CSChrActionRequestModule` exactly like a real press (requests,
new press, hold timer, release), but the player still doesn't dodge, for any hold length (4 to 30 frames). The idle gating masks are the same for a
real tap and ours. So the behavior reads input from another place, most likely the player's pad manipulator (untyped). Hidden, the request
doesn't even reach the module. Details and next candidates: `docs/research/elden-ring-input.md` ("session 2 results"). The next step needs new
ER struct offsets, so plan it first.
