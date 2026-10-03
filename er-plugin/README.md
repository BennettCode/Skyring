# er-plugin/: Elden Ring hidden-game plugin (`skyrimxer_er.dll`)

Rust `cdylib` using **eldenring-rs** (fallback: libER in C++, decided in Phase 0). Loaded **only** by me3 through
`me3/skyrim-x-er.me3`. Never copied into the game folder, never injected any other way. Elden Ring runs **offline (no EAC)**.

Planned layout (Phase 1):
```
er-plugin/
├── Cargo.toml              crate name: skyrimxer-er, crate-type = ["cdylib"]
└── src/
    ├── lib.rs              DllMain / me3 entry, logging (tracing → file), spawn the init task
    ├── bridge.rs           shared-memory server (uses protocol/generated/skyrimxer_protocol.rs)
    ├── window.rs           hide window + focus spoof
    ├── arena.rs            warp test character to the arena, spawn dummy targets
    ├── input.rs            feed Skyrim input into ER's input/action system
    ├── state.rs            read PlayerState (hp/fp/stamina/action/anim/iframe/poise)
    └── combat.rs           damage calculation for Skyrim hits (Phase 5)
```
The workspace root `Cargo.toml` (created in Phase 1) includes `er-plugin` and `tools/protogen`.
Game code runs on the game thread through eldenring-rs task registration. Never touch game state from a random thread.
