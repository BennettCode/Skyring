# MODLOG

Newest first. One entry per session or verified step: **what changed · how it was tested · result · follow-ups**.

---

## 2026-10-04: Phase 3 started: ER runs hidden; dodge injection research
- **Changed:** P3 plan (`docs/P3-PLAN.md`). `er-plugin`: `window.rs` (each frame: focus flags spoofed; hide the window while the player is in
  the world, show it again at the title), `game.rs` (player snapshot: HP/FP/stamina, anim, i-frame/dodging/hyperarmor flags, poise, position,
  yaw, map), `config.rs` (`skyrimxer_er.cfg` next to the DLL), `actions.rs` + `pad.rs` (dodge self-test + frame-phase probe, dev-only, off by default).
  `tools/dev.ps1`: `-Restart`, `-ErVisible`, `-ErSelfTest`, `-ErInjectGroup`, `-ErProbe`. `tools/build.ps1`: retries the DLL copy after a kill.
- **Tested in-game (ER only):** the window hides on load-in and ER keeps running at 60 fps hidden; the state line reads stamina 101/101, HP 455/455,
  map m10_01_00_00. Probe of a real dodge press recorded where ER sets its action bits (`docs/research/elden-ring-input.md`).
- **Result:** step 1 ✔. Step 2 (injected dodge) ✘ so far: writing `action_requests` is overwritten by the engine; a held virtual pad key is seen
  by ER's pad poll but doesn't reach the player's action requests. Next hypotheses are in the research note.

## 2026-10-04: Phase 2 complete (shared memory + heartbeat)
- **Changed:** Protocol schema v1 (`protocol/schema/messages.toml`) + generator `tools/protogen` (rejects implicit padding, checks the region
  map, emits size/offset asserts for C++ and Rust, `--check` stale test). Rust crate `skyrimxer-protocol` (region, SPSC rings, link state
  machine) used by `er-plugin` and `tools/fake-peer`. C++ mirror `skse/src/bridge/Link.cpp` + `Bridge.cpp`, and `skyrimxer_link_test.exe`.
  `tests/run-tests.ps1`. `collect-logs.ps1` now prints `[link]` lines (heartbeat lines summarised).
- **Tested without games:** `tests/run-tests.ps1` passes: 10 Rust tests (ring wrap/full/corrupt, handshake, timeout, reconnect, Bye, restart,
  header mismatch), 9 C++ selftest checks, Rust↔C++ cross-process (crash → timeout; clean exit → Bye).
- **Tested in-game (agent-run, both games at the main menu):**
  - ER created the region, Skyrim opened it on kDataLoaded: `CONNECTED: handshake ok` on both sides within 50 ms.
  - ~7 min connected: heartbeat events every 5 s both ways, 0 dropped. ER `frames` rose ~300 per 5 s (≈ 60 fps while unfocused at the title).
  - `eldenring.exe` force-killed → Skyrim: `LOST: ER ... heartbeat timeout (last beat 2031 ms ago)`, Skyrim kept running.
  - ER relaunched alone → `attach#2`, reconnected without restarting Skyrim.
  - Skyrim window closed → clean exit, ER: `LOST: Skyrim ... shutting down` + `Skyrim said Bye (reason=Quit)`, ER kept running. No crash logs.
- **Bugs fixed during the work:** Hello read before its sender was detected looked like a restart (fix: check peer before reading events, and
  answer a Hello received while connected). ER log level name `warn` → `warning` to match spdlog.
- **Next:** P3.

## 2026-10-04: Workflow + repo hygiene
- **Changed:** Added `tools/dev.ps1` (build → deploy → backup → launch → wait for plugin ready lines → collect logs). AI-agent files are now
  local-only, and history was rewritten so they never appear in it. Logging format moved to `docs/DESIGN.md` §8 and the release checklist to
  `release/README.md`, so public docs are self-contained. Added a local pre-commit guard against private data and game/binary files.
- **Tested:** `dev.ps1 -NoLaunch` (full build + deploy in 9 s). The pre-commit guard blocks a staged Windows user path and passes clean files.
  The full `dev.ps1` launch path gets its first real run in P2.

## 2026-10-04: Phase 1 complete (hello world from both plugins)
- **Changed:** Added `skse/` (CMake + Ninja preset, VS-bundled vcpkg pinned to registry baseline 00c5775, CommonLibVR-ng submodule @39f9d07,
  AE only, `src/main.cpp`). Added the Cargo workspace + `er-plugin/` (eldenring-rs @59fbd3b, `lib.rs` + `log.rs`, panic=unwind).
  Added tools: build, deploy, backup-saves, launch, collect-logs, common.psm1. Added `local/paths.json`.
- **Tested in-game (both games at once via `tools/launch.ps1`):**
  - Skyrim: `SkyrimXER v0.1.0.0 loaded, runtime 1.7.104.0` → `kDataLoaded`. skse64.log: "loaded correctly".
  - ER: `loaded` → `game version supported, task system ready` → `per-frame task registered` → user loaded the test character →
    `main player spawned (in world)` → `main player gone (menu/loading)`. me3 attach config: our native, `savefile: skyrimxer.sl2`, `start_online: false`.
- **Bugs fixed:** (1) `SKSE::Init` replaced our logger, fixed with `SKSE::InitInfo{ .log = false }`, re-tested OK. (2) build.ps1 aborted on the
  VS dev shell's harmless stderr under PS 5.1 + `Stop`, fixed by running that call with `Continue` and checking the exit code.
- **Build times:** SKSE cold 385 s / incremental 10 s. ER cold 4m39s / incremental ~2 s.
- **Next:** P2 shared memory + heartbeat.

## 2026-10-03: Phase 0 complete
- **Decided:** CommonLibVR-ng 10.1.0 (`ae` preset; has `RUNTIME_SSE_1_7_104`). eldenring-rs v0.14 (supports exe 2.7.1.0 WW). libER dropped.
  Project license **GPL-3.0-or-later** (user choice; every CommonLib supporting 1.7.104 is GPL-3.0). `LICENSE` added.
- **Found:** ER input can be injected as `ChrActions` bits on `CSChrActionRequestModule` (no OS keys). Focus flag is
  `DLUserInputManagerImpl.is_game_window_focused`. Notes in `docs/research/elden-ring-input.md`.
- **Tested:** eldenring-rs `apply-speffect` example built (29 s). CommonLib `ae` built with VS 2026 + bundled vcpkg (590 s), unit tests 25/25.
  `tools/setup-check.ps1` written and passing.
- **Moved to P3:** arena choice, movement-direction source, both-games-at-once performance.

## 2026-10-03: me3 smoke test (Elden Ring, no mods)
- **Ran:** `me3 launch -g eldenring --savefile skyrimxer.sl2` after backing up ER saves to `local/save-backups/`.
- **Result ✔:** me3 attached to "ELDEN RING 1.17.1.0 Worldwide". Hooks applied (filesystem, allocators, assets, skip_logos). Arxan detected
  and the attach deferred cleanly. No EAC process. All save I/O redirected to `skyrimxer.sl2` (a copy of ER0000 made on first use), and
  `ER0000.sl2` was untouched. Zero WARN/ERROR lines. Log: `logs/me3-smoke-stderr.txt` + `%LOCALAPPDATA%\garyttierney\me3\data\logs\transient-profile\`.
- **Next:** Phase 0 library checks (CommonLib fork for 1.7.104, eldenring-rs for 1.17.1).

## 2026-10-03: Tooling installed
- **Changed:** Installed me3 0.13.0 (official signed installer, silent). Installed Crash Logger 1.25.0 (GitHub) into `Data\SKSE\Plugins`.
  Found that the installed SKSE 2.2.6 targeted 1.6.1170, which doesn't match the game's 1.7.104. Replaced it with **SKSE 2.3.1**
  (127 overwritten files backed up to `local/backups/skse-2.2.6/`). Installed **Address Library v13 All in One** (contains `versionlib-1-7-104-0.bin`).
  Every change is listed in `local/install-manifest.json`.
- **Found:** me3 profiles can't store a save file, so launches must pass `--savefile skyrimxer.sl2`. me3 blocks matchmaking by default.
  MO2 is installed but has no instance, so files went straight into `Data\`.
- **Tested:** File versions checked (`skse64_1_7_104.dll` / loader = 2.3.1). `me3 info` → installation Found. **Not yet tested in-game.**
- **In-game check (same day):** Launched through `skse64_loader.exe`. `skse64.log`: SKSE 2.3.1 initialized, CrashLogger "loaded correctly",
  kDataLoaded reached. ✔ The Skyrim side of the toolchain works.
- **Next:** me3 smoke test with Elden Ring (no natives, `--savefile skyrimxer.sl2`), then the rest of Phase 0.

## 2026-10-03: Project scaffold
- **Changed:** Created the repo structure, local agent-rules file, design/roadmap/recon docs, whitelist `.gitignore`, me3 profile template,
  and folder READMEs. Ran `git init`. Cloned the reference repos into `reference/` (ignored).
- **Environment found:** Skyrim `SkyrimSE.exe` 1.7.104.0 + SKSE 2.2.6 (no Address Library yet). Elden Ring `eldenring.exe` 2.7.1.0.
  VS 2026 C++ ✔, Rust ✔, git ✔. Missing: me3, Address Library.
- **Decision:** Skyrim plugin uses SkyCraft's CMake + `alandtse/CommonLibVR` (ng) submodule setup, so xmake isn't needed. Fake-peer test processes are planned for P2.
- **Tested:** `git status --ignored` confirms only docs/config are tracked and game/build/local folders are ignored.
- **Next:** Phase 0 (checking tools & versions). See `STATUS.md`.
