# ROADMAP

Rule: don't start a phase until the previous phase's acceptance check is **confirmed by the user in-game** and committed.
Tick boxes as you go. Keep each phase small. If a phase grows, split it.

## P0: Checking tools & versions
- [x] `docs/RECON.md` sections A, B, D complete (C = both-games-at-once numbers moved to P3, which runs both for real)
- [x] Missing tools installed (SKSE 2.3.1, Address Library v13, Crash Logger, me3 0.13.0)
- [x] `tools/setup-check.ps1` passes (2026-10-03)
- [x] CommonLib `ae` build succeeds with VS 2026 + bundled vcpkg (590 s cold, `[unit]` tests 25/25 pass, 2026-10-03)
- [x] User creates a dedicated ER test character in `skyrimxer.sl2` (2026-10-03)
**Accept:** the user approves the decisions in RECON §D.

## P1: Hello world (both sides)
- [x] `skse/`: plugin builds. On load it writes `SkyrimXER v0.1.0.0 loaded, runtime 1.7.104.0`, then `kDataLoaded`
- [x] `er-plugin/`: cdylib builds and gets loaded by me3. Logs `loaded`, `game version supported, task system ready`, `per-frame task registered`,
  and `main player spawned/gone` (proves eldenring-rs reads `WorldChrMan` on this exe)
- [x] `tools/build.ps1`, `deploy.ps1` (+`-Undo`, `-WhatIf`), `backup-saves.ps1`, `launch.ps1`, `collect-logs.ps1`
**Accept:** ✔ **DONE 2026-10-04.** Both log lines present after one `tools/launch.ps1` run with both games at once. Logs in `logs/20261004-000407/`.

## P2: Shared memory + heartbeat
- [ ] `protocol/schema` v1 header + Hello/Bye. Generator writes `.h` + `.rs`. Layout tests pass on both sides
- [ ] `fake-skyrim` / `fake-er` stand-in processes (Rust, `tools/`) so each side can be tested without the other game
- [ ] Both sides open the region, exchange Hello, and log the other side's heartbeat every 5 s
- [ ] Kill ER → Skyrim logs a timeout and stays stable. Kill Skyrim → ER goes idle
**Accept:** the logs show the handshake, heartbeats and a clean timeout.

## P3: One value across, then the loop
- [ ] ER: window hidden + focus spoof. Test character warped to the arena
- [ ] Skyrim → ER: forward one input (dodge button). ER performs a roll. ER log: stamina before/after
- [ ] ER → Skyrim: PlayerState slot (stamina, action_state, iframe). Skyrim logs it at 1-in-30 frames
- [ ] Coordinate/yaw conversion test written and passing
**Accept:** pressing dodge in Skyrim → ER rolls → Skyrim's log shows the stamina drop and the i-frame window with timestamps.

## P4: Movement & defense
- [ ] Swallow vanilla Skyrim dodge/sprint/block/attack input while the bridge is on (F10 toggle)
- [ ] Skyrim player follows ER's roll movement curve. Rolls play an animation in Skyrim
- [ ] Skyrim's stamina bar mirrors ER's stamina. Out of stamina = no roll
- [ ] NPC hits during ER i-frames are cancelled
**Accept:** the user rolls through an NPC's attack without taking damage, and spamming rolls is limited by stamina.

## P5: Combat bridge
- [ ] Weapon mapping table (Skyrim weapon → ER weapon ID) in `config/`
- [ ] Player light/heavy attacks: ER timing + damage math → applied to Skyrim actors (HitResult)
- [ ] NPC → player damage goes through ER defenses. ER HP is the authority. Skyrim HP mirrors it
- [ ] Poise/stagger both ways. Logs show every damage calculation input
**Accept:** damage numbers in the logs match ER's formula. Staggers happen when they should.

## P6: HUD
- [ ] ER-style HP/FP/stamina bars, flask counts, rune counter. Vanilla bars hidden while the bridge is on
**Accept:** the HUD tracks live values with no visible lag.

## P7: Progression
- [ ] Runes from Skyrim kills (scaled by actor level). Rune loss on death + bloodstain (optional)
- [ ] Flasks refill when resting (bed/shrine = "grace"). Leveling through a menu
**Accept:** one full loop: fight → earn runes → rest → level up.

## P8: Visuals & performance (stretch goal)
- [ ] Frame-time profiling on both sides. Budget: 60 fps on the user's PC
- [ ] Optional: ER player/VFX rendered offscreen and depth-composited into Skyrim
**Accept:** profiling numbers logged in MODLOG. The user is happy with how it feels.
