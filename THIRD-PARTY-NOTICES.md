# Third-party notices and credits

This project is licensed **GPL-3.0-or-later** (see `LICENSE`).

List every project whose code, design or tooling this repo uses, with its license. Add entries in the same commit that introduces the dependency.
If code is copied, also keep the original license text in `licenses/<project>.txt`.

| Project | Used for | License | Link |
|---|---|---|---|
| SkyCraft (chasmlol) | Reference architecture: shared-memory protocol, authority split, frame lockstep, CMake/CommonLib setup, fake-peer testing | MIT | https://github.com/chasmlol/SkyCraft |
| FalloutCraft (zeyvu) | Reference port of the SkyCraft design | MIT | https://github.com/zeyvu/FalloutCraft |
| GTA San AnSkateas (ryglizzy) | Reference: plugin + Rust FFI engine bridge (study only; no LICENSE file, so **don't copy code**) | none found | https://github.com/ryglizzy/GTA-San-AnSkateas |
| AI Game Modding Guides (trevaintdead) | Workflow, rules and publishing guidance | MIT | https://github.com/trevaintdead/ai-game-modding-guides |
| CommonLibVR, `ng` branch (alandtse) | Skyrim plugin library (git submodule) | GPL-3.0-or-later WITH Modding Exception + GPL-3.0 Linking Exception (since 2026-08-20) | https://github.com/alandtse/CommonLibVR |
| CommonLib vcpkg deps: spdlog, fmt, xbyak, SimpleIni, rapidcsv, toml11, nlohmann-json, DirectXTK/DirectXMath | Built into the Skyrim plugin | MIT / BSD-style (check each when packaging) | via vcpkg |
| SKSE64 | Skyrim script extender (runtime requirement, not redistributed) | SKSE license | https://skse.silverlock.org |
| Address Library for SKSE Plugins | Version-independent addresses (runtime requirement, not redistributed) | Nexus permissions | https://www.nexusmods.com/skyrimspecialedition/mods/32444 |
| me3 | Elden Ring mod loader (runtime requirement, not redistributed) | Apache-2.0 / MIT | https://github.com/garyttierney/me3 |
| eldenring-rs / fromsoftware-rs (vswarte) | Elden Ring native bindings (Rust crates `eldenring`, `fromsoftware-shared`) | MIT OR Apache-2.0 | https://github.com/vswarte/eldenring-rs |

Game content belongs to its owners: *The Elder Scrolls V: Skyrim* © Bethesda Softworks/ZeniMax. *ELDEN RING* © FromSoftware/Bandai Namco.
This repository contains none of it.
