# Third-party notices and credits

This project is licensed **GPL-3.0-or-later** (see `LICENSE`).

List every project whose code, design or tooling this repo uses, with its license. Add entries in the same commit that introduces the dependency.
If code is copied, also keep the original license text in `licenses/<project>.txt`.

| Project | Used for | License | Link |
|---|---|---|---|
| SkyCraft (chasmlol) | Reference architecture: shared-memory protocol, seqlock slot pattern, PlayerCharacter::Update hook + input sink pattern, authority split, frame lockstep, CMake/CommonLib setup, fake-peer testing; **code adapted:** the stamped-history interpolation with an adaptive render delay (`Game.cpp:662-760` → `skse/src/bridge/Timeline.cpp`) the melee hit call-site replacement with its byte check (`Combat.cpp` ResolveHitPipeline → `skse/src/hooks/PlayerHit.cpp`) the HitData-through-Skyrim's-hit-function apply (`Combat.cpp` ApplyHit → `skse/src/bridge/Combat.cpp`) and the player-damage refund/forward, hit attribution and kill (`Combat.cpp` BridgePlayerDamage, HitSink, KillPlayer → `skse/src/bridge/Health.cpp`), license in `licenses/SkyCraft.txt` | MIT | https://github.com/chasmlol/SkyCraft |
| FalloutCraft (zeyvu) | Reference port of the SkyCraft design; **code adapted:** the host-bar fraction mirror through the damage modifier (`FO4_ModFiles/fo_combat.cpp:151-188` → `skse/src/bridge/Stamina.cpp`, license in `licenses/FalloutCraft.txt`) | MIT | https://github.com/zeyvu/FalloutCraft |
| GTA San AnSkateas (ryglizzy) | Reference: plugin + Rust FFI engine bridge (study only; no LICENSE file, so **don't copy code**). Idea re-implemented in our own code: pad-input test scripts (`skse/src/bridge/PadScript.*`) | none found | https://github.com/ryglizzy/GTA-San-AnSkateas |
| Killcraft (goonsn) | Reference: host-side patterns (tick interpolation, damage forwarding, input hand-back, overlay) | MIT | https://github.com/goonsn/Killcraft |
| 2010-rust-rewrite-mashup (vladtrc, chasmlol) | Reference: skeleton pose retargeting (`render_anim/src/skate/rig.rs`) and AI agent workflow; its bind-delta retarget idea is re-implemented (no code copied) in `protocol/src/rig.rs` (`docs/POSE-PLAN.md`) | Apache-2.0 (its `skate/` and `third_party/` folders: no license stated, study only) | https://github.com/chasmlol/2010-rust-rewrite-mashup |
| AI Game Modding Guides (trevaintdead) | Workflow, rules and publishing guidance | MIT | https://github.com/trevaintdead/ai-game-modding-guides |
| CommonLibVR, `ng` branch (alandtse) | Skyrim plugin library (git submodule) | GPL-3.0-or-later WITH Modding Exception + GPL-3.0 Linking Exception (since 2026-08-20) | https://github.com/alandtse/CommonLibVR |
| CommonLib vcpkg deps: spdlog, fmt, xbyak, SimpleIni, rapidcsv, toml11, nlohmann-json, DirectXTK/DirectXMath | Built into the Skyrim plugin | MIT / BSD-style (check each when packaging) | via vcpkg |
| SKSE64 | Skyrim script extender (runtime requirement, not redistributed) | SKSE license | https://skse.silverlock.org |
| Address Library for SKSE Plugins | Version-independent addresses (runtime requirement, not redistributed) | Nexus permissions | https://www.nexusmods.com/skyrimspecialedition/mods/32444 |
| me3 | Elden Ring mod loader (runtime requirement, not redistributed) | Apache-2.0 / MIT | https://github.com/garyttierney/me3 |
| eldenring-rs / fromsoftware-rs (vswarte) | Elden Ring native bindings (Rust crates `eldenring`, `fromsoftware-shared`) | MIT OR Apache-2.0 | https://github.com/vswarte/eldenring-rs |
| Rust crates: `windows` (Microsoft), `chrono`, `serde`, `toml` | Win32 bindings + log timestamps in the ER plugin and protocol crate; schema parsing in `tools/protogen` (build tool only) | MIT OR Apache-2.0 | https://crates.io

Game content belongs to its owners: *The Elder Scrolls V: Skyrim* © Bethesda Softworks/ZeniMax. *ELDEN RING* © FromSoftware/Bandai Namco.
This repository contains none of it.
