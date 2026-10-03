# RECON: Phase 0 results

Fill in every item **before** writing any plugin code. Each answer needs a source (link, file, log line or test).
This phase is research only: no code yet, apart from throwaway tests in `local/`.

## A. Skyrim (host)
- [ ] Exact runtime: `SkyrimSE.exe` 1.7.104.0 confirmed (in-game version string: ____)
- [x] SKSE for 1.7.104: **2.3.1** installed (skse.silverlock.org lists 2.3.1 for 1.7.104). The old 2.2.6 was for 1.6.1170. **Loads ✔** (skse64.log 2026-10-03, reached kDataLoaded)
- [x] Address Library for 1.7.104: **v13 All in One**, `versionlib-1-7-104-0.bin` installed
- [ ] Does `alandtse/CommonLibVR` (branch `ng`, what SkyCraft uses) support 1.7.x? Commit: ____. If not, which fork does: ____
- [x] Toolchain proven: CommonLibVR-ng preset `build-release-msvc-vcpkg-ae` builds from the VS Developer Shell (CMake 4.3, Ninja, VS-bundled
  vcpkg at `VC\vcpkg`). Cold build 590 s (vcpkg deps + 532 steps). `CommonLibSSETests.exe [unit]` passes 25/25. Log: `logs/commonlib-ae-build.log`
- [x] Mod manager: MO2 is installed (`C:\Modding\MO2`) but has **no instance**, so dev files go straight into `Data\` (tracked in `local/install-manifest.json`)
- [ ] Other SKSE plugins installed that could conflict (combat overhauls, animation frameworks like OAR/DAR, MCO/BFCO, TDM, True Directional Movement): ____
- [x] Crash Logger 1.25.0 installed (its changelog mentions 1.7.99; confirm it loads on 1.7.104 in `skse64.log`): **loads correctly ✔**

## B. Elden Ring (hidden game)
- [x] `eldenring.exe` 2.7.1.0 → **ELDEN RING 1.17.1.0 Worldwide** (me3 attach log) (DLC installed? ____)
- [x] eldenring-rs supports this patch: **yes**, `rva.rs` maps exe `2.7.1.0` (EN/WW) → `Ww2710`. Commit `59fbd3b` (2026-09-20), crates v0.14.0,
  stable Rust. Example `apply-speffect` builds in 29 s. libER: last update for patch 1.16 (Apr 2025), so **dropped**
- [x] DLC (Shadow of the Erdtree) installed: **yes** (`DLC.bdt` present)
- [x] me3 **0.13.0** installed · CLI: `me3 launch -g eldenring -p me3/skyrim-x-er.me3 --savefile skyrimxer.sl2` · logs: `%LOCALAPPDATA%\garyttierney\me3\data\logs`
- [x] me3 launches **without EAC** (no EasyAntiCheat/start_protected_game process). Save I/O is redirected to `skyrimxer.sl2` (seeded as a copy
  of ER0000.sl2 on first use), and ER0000.sl2 was untouched. Arxan detected; me3 defers its attach. No warnings/errors (smoke test 2026-10-03)
- [~] Hidden/unfocused: eldenring-rs exposes `DLUserInputManagerImpl.is_game_window_focused` → plan is to force it true each frame.
  Run ER **windowed** (exclusive fullscreen + alt-tab is known to stutter). **Test in P3.** See `docs/research/elden-ring-input.md`
- [ ] Arena location: **decide in P3** with a warp test. Default candidate: Roundtable Hold (enclosed, no hostile enemies). The plugin can
  also remove nearby enemies if needed
- [ ] Test character: name ____, level ____, build ____ (dedicated slot in `skyrimxer.sl2`). **User to create**
- [x] Input injection mechanism: **set bits in `CSChrActionRequestModule.action_requests` (ChrActions)** during `ChrIns_PreBehavior`. No OS keystrokes.
  Bits exist for r1/r2/l1/l2/sp_move(roll)/jump/use_item/guard/two-hand. Movement direction source is still open (P3)

## C. Both running together
- [ ] VRAM/RAM/CPU with both running (Task Manager numbers): ____
- [ ] Frame rate with ER hidden + Skyrim in the foreground: ____
- [ ] Shared memory region test (two tiny programs, or one throwaway DLL): works across both processes? ____

## D. Decisions
- [x] **CommonLib:** `alandtse/CommonLibVR` branch `ng` (v10.1.0, 2026-09-30). Defines `RUNTIME_SSE_1_7_104` = `RUNTIME_SSE_LATEST_AE`.
  1.7.104 support synced from powerof3/CommonLibSSE on 2026-09-08. Build preset `ae` only. Deps through vcpkg bundled with VS 2026.
- [x] **License:** project is **GPL-3.0-or-later** (user decision 2026-10-03). Every CommonLib supporting 1.7.104 is GPL-3.0
  (alandtse relicensed 2026-08-20; powerof3 is GPL-3.0). `LICENSE` added.
- [x] **ER binding:** eldenring-rs (Rust, v0.14, supports exe 2.7.1.0 WW). libER dropped (stuck on patch 1.16).
- [x] **Authority table v1:** unchanged from DESIGN.md §3, with one refinement: ER's own action gating
  (`possible_action_inputs` / `disabled_action_inputs`) decides whether an injected action happens. Skyrim never forces an action.
- [x] **Coordinate test plan (P3):** in the arena, the ER plugin logs `main_player` position/yaw each frame while the user walks the ER character
  N → E → up a step. In Skyrim, the SKSE plugin logs player position/heading while walking the same directions. Compare signs and axes →
  write the mapping + a unit test in `tests/`. Only *deltas* are used (ER world position is never shown).
