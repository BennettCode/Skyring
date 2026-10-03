# tools/: dev scripts

All scripts: PowerShell 5.1-compatible, `Set-StrictMode -Version Latest`, `$ErrorActionPreference = 'Stop'`, read paths from
`local/paths.json` (template: `config/paths.example.json`), and support a `-WhatIf`-style dry run when they write outside the repo.

| Script | Phase | Does |
|---|---|---|
| `dev.ps1` | P1 | **The dev loop in one command:** build → deploy → back up saves → launch → wait for both plugins' ready lines → collect logs. Flags: `-Target all\|skse\|er`, `-Game both\|eldenring\|skyrim`, `-NoLaunch`, `-WaitSeconds`. |
| `setup-check.ps1` | P0 | Checks game exe versions, SKSE, Address Library, me3, xmake, VS C++ tools, Rust. Prints ✔/✖ with fix hints. |
| `backup-saves.ps1` | P1 | Copies the ER `.sl2` + Skyrim `Saves/` into `local/save-backups/<timestamp>/`. Keeps the last N. |
| `build.ps1` | P1 | Builds skse + er-plugin (+ protogen), copies outputs into `build/`. |
| `deploy.ps1` | P1 | Copies `SkyrimXER.dll/.pdb` into `Data/SKSE/Plugins/` and writes `local/deploy-manifest.json`. `-Undo` removes exactly those files. |
| `launch.ps1` | P1–P3 | Backup → `me3 launch` ER (hidden) → wait for Ready → `skse64_loader.exe`. |
| `collect-logs.ps1` | P1 | Copies SkyrimXER.log, skse64.log, crash logs, ER plugin log, me3 log → `logs/<timestamp>/`. |
| `protogen/` | P2 | Rust crate: `protocol/schema` → `protocol/generated`. |
| `fake-skyrim/`, `fake-er/` | P2 | Rust stand-in processes that act as the other side (Hello, heartbeat, scripted inputs/state), so each plugin can be tested alone. Based on SkyCraft's `fake_guest.py`/`fake_skyrim.py`. |
| `merge-logs.ps1` | P3 | Merges both sides' telemetry by timestamp for analysis. |
