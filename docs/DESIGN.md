# DESIGN: Skyrim X Elden Ring

Plain-language architecture. **Status: DRAFT v0.** Every "decision" marked ⚠ still needs a Phase 0/3 test before it's final.
Record *why* next to each decision so future sessions don't argue about it again.

## 1. Goal
Skyrim's world, NPCs and quests, played with Elden Ring's player combat. Neither game is rewritten. They run side by side and
exchange state (passthrough mod, based on SkyCraft).

## 2. Components

| Component | Where | Lang / stack | Output |
|---|---|---|---|
| Host plugin | `skse/` | C++23, CommonLibSSE-NG (submodule), CMake, spdlog | `SkyrimXER.dll` → `Skyrim/Data/SKSE/Plugins/` |
| Hidden-game plugin | `er-plugin/` | Rust cdylib, eldenring-rs v0.14 (git, pinned commit), tracing | `skyrimxer_er.dll` (loaded by me3 from `build/`) |
| Protocol | `protocol/` | TOML schema → generated `.h` + `.rs` | compiled into both plugins |
| Launcher + tools | `tools/` | PowerShell 5.1-compatible | scripts |
| me3 profile | `me3/skyrim-x-er.me3` | TOML | used by `me3 launch` |

## 3. Authority (⚠ draft)

| Domain | Owner | Reason |
|---|---|---|
| World, collision, NPC AI, quests | Skyrim | It's the host. ER has no copy of Skyrim's geometry. |
| Player position integration | Skyrim ⚠ | ER can't collide with Skyrim terrain. ER supplies movement *intent* (roll distance curve, attack root motion). **Test:** does a roll feel like ER when Skyrim moves the player along ER's curve? |
| Player input | Skyrim captures → ER | The player is looking at the Skyrim window. ER receives actions as `ChrActions` bits written into `CSChrActionRequestModule.action_requests` (no OS keystrokes). ER's own gating (`possible_action_inputs`) decides whether they happen. See `docs/research/elden-ring-input.md`. |
| Stamina/FP/HP math, i-frames, poise, attack timing, recovery | ER | This is the gameplay we're importing. |
| Player→NPC damage | ER calculates, Skyrim applies | Skyrim knows *what* got hit. ER knows *how much*. Applying it in Skyrim keeps stagger, crime and AI reactions. |
| NPC→player damage | Skyrim detects, ER applies | ER's i-frames/defense/HP must decide the outcome. |
| Death | ER HP | |

Alternatives considered (if the draft fails the tests):
- **ER owns position too** (like SkyCraft's Minecraft): would need Skyrim collision streamed into ER's Havok world. Very hard, deferred.
- **No hidden ER, re-implement the rules in SKSE**: simpler, but it's not a merge and loses ER's exact timing/math. Fallback only.

## 4. Protocol (`Local\SkyrimXER_v1`)

```
SharedRegion {
  Header        { magic 'SXER', u32 version, u32 header_size, u32 sky_pid, u32 er_pid,
                  u64 sky_heartbeat_ms, u64 er_heartbeat_ms, u32 sky_state, u32 er_state }
  Slot<InputState>    sky→er   seqlock   per Skyrim frame   (buttons, sticks, camera yaw/pitch)
  Slot<CameraState>   sky→er   seqlock   per Skyrim frame
  Slot<PlayerState>   er→sky   seqlock   per ER frame       (hp, fp, stamina, maxes, action_state, anim_id,
                                                              iframe, hyperarmor, poise, move_delta_xyz, yaw)
  Ring<Event> sky→er  (Hello, Bye, PlayerHurt{dmg types, attacker level, hit dir}, Toggle, ...)
  Ring<Event> er→sky  (Hello, Bye, HitResult{target formid, dmg, poise dmg, status}, FlaskUsed, RunesChanged, Died, ...)
}
```
Rules: fixed-size little-endian plain structs, explicit padding, size/offset asserts on both sides, schema is the single source of truth,
`version` bump for any layout change. Heartbeat timeout of 2 s → fail-safe (Skyrim goes back to vanilla combat).

## 5. Launch / lifecycle
1. `tools/launch.ps1` → back up saves → `me3 launch` ER with the profile (EAC never starts, offline only).
2. ER plugin: open region → hide window + focus spoof → load test character → warp to arena → `Ready`.
3. `skse64_loader.exe` → Skyrim plugin connects on `kDataLoaded` → `Hello` handshake (versions must match).
4. Save loaded → bridge active (F10 toggles). Either side quitting → `Bye` → the other side goes idle.

## 6. Coordinates (⚠ to be tested)
Skyrim Z-up, 70 u ≈ 1 m. ER Y-up, 1 u = 1 m. Only *deltas* in the arena matter. Handedness and yaw zero are unknown.
Test: walk N/E/up in each game and log both, then write the conversion + a unit test in `tests/`.

## 7. Animation / visuals
- v1: Skyrim plays its own (or custom-imported, user-supplied) animations chosen from ER's `action_state`/`anim_id`
  through a mapping table. The ER character is never seen.
- Phase 8 stretch goal: share ER's render of the player/VFX as a texture (D3D12 shared handle → D3D11, fence), depth-composited into Skyrim.

## 8. Logging format (both plugins)
Every line, on both sides:
```
<UTC ISO-8601 time, ms> [SKY|ER] [level] [subsystem] message
2026-10-03T23:03:13.755Z [ER] [info] [core] main player spawned (in world)
```
- Skyrim plugin → `Documents\My Games\Skyrim Special Edition\SKSE\SkyrimXER.log`. ER plugin → `build\er-plugin\logs\skyrimxer_er.log`.
  Both are truncated on each game start. `tools/collect-logs.ps1` gathers them (plus skse64/crash/me3 logs) into `logs/<timestamp>/`.
- Same format on both sides so the two logs can be merged by timestamp. Log state *changes* and events, sample per-frame values (1 in N),
  and log frame times as p50/p95/p99 every 5 s. Never log every frame.

## 9. Open questions
- Can ER be kept running at full rate while hidden/unfocused? What's the cost when both games run at once?
- What's the best hook point in Skyrim to cancel incoming player damage and redirect it?
- How do we take Skyrim's "hit happened" and get ER's damage formula result without a real ER target? Options: a dummy enemy in the arena
  with matching defenses, or calling ER's damage calculation directly through eldenring-rs.
- Mapping Skyrim weapons to ER weapons (EquipParamWeapon IDs). This needs a mapping table in `config/`.
