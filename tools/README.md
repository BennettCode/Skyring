# tools/: dev scripts

All scripts: PowerShell 5.1-compatible, `Set-StrictMode -Version Latest`, `$ErrorActionPreference = 'Stop'`, read paths from
`local/paths.json` (template: `config/paths.example.json`), and refuse or dry-run (`-WhatIf`) when they would write outside the repo.
Run them as `powershell -ExecutionPolicy Bypass -File tools/<script>`. Shared helpers: `common.psm1`.

| Script | Does |
|---|---|
| `dev.ps1` | **The dev loop in one command:** build → deploy → back up saves → launch → wait for each plugin's ready line → collect logs. See the flags below. |
| `stop-games.ps1` | Closes Skyrim and/or ER (`-Game both\|eldenring\|skyrim`): clean close first, then kill (ER is hidden in-world, so it gets killed). |
| `addrlib-check.ps1` | `-Id 208040,402776`: checks Address Library ids against the installed `versionlib-*.bin` (format 5). Run it before using any new id: a missing one aborts Skyrim at load. |
| `setup-check.ps1` | Read-only check of game exe versions, SKSE, Address Library, Crash Logger, me3, VS C++/CMake/vcpkg, Rust, git, and that EAC isn't running. |
| `build.ps1` | `-Target all\|skse\|er`. skse: CMake in a VS dev shell → `build/skse/`. er: cargo → `build/er-plugin/` (retries the copy while a killed ER holds the DLL). |
| `deploy.ps1` | Copies `SkyrimXER.dll/.pdb` into `Data/SKSE/Plugins/` + writes `local/deploy-manifest.json`. `-Undo` removes exactly those files. `-WhatIf`. |
| `launch.ps1` | Backs up saves, starts ER through me3 (offline, dev save `skyrimxer.sl2`), then Skyrim through `skse64_loader.exe`. |
| `backup-saves.ps1` | Both games' saves → `local/save-backups/<timestamp>/`, keeps the newest 10. |
| `collect-logs.ps1` | Copies both plugin logs, skse64.log, the newest crash log and the me3 log into `logs/<timestamp>/`, prints `[core]`/`[link]`/`[error]` lines. |
| `protogen/` | Rust: `protocol/schema/messages.toml` → `protocol/generated/`. `cargo run -p protogen [-- --check]`. |
| `fake-peer/` | Rust stand-in for either game on the link: `fake-peer <skyrim\|er> [--seconds N] [--no-bye] [--region NAME]`. |

## `dev.ps1` flags

| Flag | Effect |
|---|---|
| `-Target all\|skse\|er` | What to build (default all). |
| `-Game both\|eldenring\|skyrim` | What to launch (default both). |
| `-NoLaunch` | Build + deploy only. |
| `-Restart` | Close the game(s) about to be launched first. |
| `-WaitInWorld N` | After ER reports in, wait for the player to press Continue (up to 5 min), let ER run N seconds, then print the ER log's `-Show` lines. |
| `-Show <regex>` | Subsystems printed by `-WaitInWorld` (default `action\|state\|window\|probe\|error\|warning`). |
| `-ErVisible` | Keep ER's window visible in-world (debugging). |
| `-ErSelfTest dodge` | ER presses dodge by itself every 4 s and logs what happens (no Skyrim needed). |
| `-ErProbe` | Log ER's dodge action bits at 9 points in each frame. |
| `-ErInjectGroup <g>` | Task group for the self-test's input write: `wprep` (default), `padstep`, `ailogic`, `prebehavior`. |

The `-Er*` flags are written to `build/er-plugin/skyrimxer_er.cfg` on every launch, so nothing lingers from an earlier run.
