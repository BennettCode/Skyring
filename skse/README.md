# skse/: Skyrim host plugin (`SkyrimXER.dll`)

C++23 SKSE plugin built on CommonLibSSE-NG, built with **CMake** (ships with VS 2026) + MSVC, following SkyCraft's setup
(`reference/SkyCraft/skse/CMakeLists.txt`). Phase 0 confirms the fork supports Skyrim 1.7.104 (see `docs/RECON.md`).

Planned layout (Phase 1):
```
skse/
├── CMakeLists.txt          add_commonlibsse_plugin(SkyrimXER ... USE_ADDRESS_LIBRARY ...)
├── extern/CommonLibSSE-NG  git submodule: alandtse/CommonLibVR, branch ng
├── src/
│   ├── main.cpp            SKSEPluginLoad, logging setup, messaging listener
│   ├── bridge/             shared-memory client (uses protocol/generated/skyrimxer_protocol.h)
│   ├── hooks/              one file per hook, each with a comment: what it intercepts + why
│   ├── combat/             applying ER results: damage, stagger, i-frames, stamina mirror
│   ├── input/              capturing and swallowing input
│   ├── anim/               action_state → animation mapping
│   └── hud/                Phase 6
└── include/                PCH.h, shared headers
```
Rules: Address Library IDs only (no raw offsets). Log to `Documents/My Games/Skyrim Special Edition/SKSE/SkyrimXER.log`.
Never block the game thread waiting on ER for more than a small timeout.
