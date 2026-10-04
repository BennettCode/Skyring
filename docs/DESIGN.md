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

## 4. Protocol (`Local\SkyrimXER_v3`)

Source of truth: `protocol/schema/messages.toml` → `cargo run -p protogen` → `protocol/generated/skyrimxer_protocol.{h,rs}`.
Fixed-size little-endian plain structs, explicit padding only (protogen rejects implicit padding), size/offset asserts in both
languages, `version` + region name bump for any layout change.

**v3 region (P4, 0x21000 bytes; v3 = v2 + InputState `flags` + PlayerFlag InCombat; v1 = v2 without the slots):**
```
0x00000 Header       magic 'SXER' (written last by the creator), version, header_size, region_size,
                     sky_/er_ pid, state (SideState), heartbeat_ms (GetTickCount64), attach_count   (64 B)
0x00100 RingHeader   sky→er   write_pos, read_pos (u64 byte totals, never wrap), capacity, dropped
0x00140 RingHeader   er→sky
0x00200 InputState   sky→er   seqlock slot (48 B): frame, time_ms, buttons (Button bits), flags (InputFlag: InCombat,
                              BridgeOn), move_x/y, cam_yaw
0x00300 PlayerState  er→sky   seqlock slot (80 B): flags (PlayerFlag bits), frame, time_ms, hp/fp/stamina + maxes,
                              anim_id, block_id, poise(+max), pos[3], yaw
0x01000 ring data    sky→er   64 KiB
0x11000 ring data    er→sky   64 KiB
```
- **Either side may create the region.** The creator fills the shared fields and publishes `magic` last (release). An opener waits for it,
  then checks magic/version/sizes. On a mismatch it logs `PROTOCOL MISMATCH` once and writes nothing. Each side writes only its own `sky_*`/`er_*` fields.
- **Rings:** exactly one writer and one reader each. Message = `{u16 msg_type, u16 size, u32 seq}` + payload, padded to 8 B, may wrap.
  A full ring drops the message and counts it in `dropped`. On (re)attach, a reader skips stale bytes and the writer restarts `seq` at 1.
- **Slots** (`protocol/src/slot.rs`, mirrored in `skse/src/bridge/Slot.h`): latest value only, one writer (that side's game thread),
  any readers. The struct starts with `seq` (odd = being written). Writer: seq+1, release fence, copy, seq+2 (release). Reader: seq (acquire),
  retry while odd, copy, acquire fence, accept if unchanged (≤ 64 tries). The body is copied as u32 atomics, so there are no data races. A new writer
  turns a dead writer's odd seq even again. **Stale** = `time_ms` older than 250 ms or the link not connected: the reader acts on nothing.
  Game threads reach the slots through `LinkShared` (region + `connected` atomic), never the link's lock. The region is only published after a
  successful join, so a mismatched layout never gets slot writes.
- **Events (unchanged since v1):** `Hello` (protocol version, pid, plugin + game version), `Bye` (reason), `Heartbeat` (uptime, frames; every 5 s).
- **Link state machine** (`protocol/src/link.rs`, mirrored in `skse/src/bridge/Link.cpp`; same steps and log wording), ticked by a
  dedicated 50 ms thread on each side, not by the game loop:
  attach (retry 1 s) → write own heartbeat → check peer → read events → maybe send a Heartbeat event.
  - Peer **alive** = state Starting/Ready/Running and heartbeat ≤ 2 s old. A **new peer** (became alive, or its attach count changed) gets a Hello.
    Its Hello back = **CONNECTED**. A Hello received while connected means the peer lost us, so we answer it.
  - **LOST** (fail-safe → idle, keep waiting): heartbeat timeout, peer state ShuttingDown/Faulted, or Bye.
  - Bye is best effort at process exit (DLL detach, never blocks or logs there). A crash is covered by the timeout.
- Why a thread, not the frame task: the heartbeat must keep going through loading screens and ER's unfocused pauses. Whether ER's game loop
  is advancing shows up separately as `frames` in its Heartbeat events.

**Planned (P3+, will bump the version):**
```
Slot<CameraState>   sky→er   seqlock   per Skyrim frame
PlayerState additions: action_state, move_delta_xyz
Events sky→er: PlayerHurt{dmg types, attacker level, hit dir}, Toggle, ...
Events er→sky: HitResult{target formid, dmg, poise dmg, status}, FlaskUsed, RunesChanged, Died, ...
```

## 5. Launch / lifecycle
1. `tools/launch.ps1` → back up saves → `me3 launch` ER with the profile (EAC never starts, offline only).
2. ER plugin: open region → hide window + focus spoof → load test character → warp to arena → `Ready`.
3. `skse64_loader.exe` → Skyrim plugin connects on `kDataLoaded` → `Hello` handshake (versions must match).
4. Save loaded → bridge active (F10 toggles). Either side quitting → `Bye` → the other side goes idle.

## 6. Coordinates (measured 2026-10-04)
Only **deltas in the character's own frame** cross over: world delta → (forward, right, up) in metres with one game's facing angle → world
delta in the other game with its facing angle. Code: `protocol/src/coords.rs` (+ C++ mirror `skse/src/bridge/Coords.h`, same tests).
- Skyrim: Z up, 70 u ≈ 1 m. Heading h (`GetAngleZ()`): forward = (sin h, cos h, 0), right = (cos h, −sin h, 0).
- ER: Y up, 1 u = 1 m. Yaw y: forward = (−sin y, 0, −cos y), right = (−cos y, 0, sin y).
- Both angles grow when turning right, so yaw deltas carry over unchanged. Method + data: `docs/research/coordinates.md`.

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
- **Controller (all testing uses a DualSense over USB).** ER reads it natively (libScePad). Skyrim is XInput-only and sees it only through Steam
  Input, which may not apply when `skse64_loader.exe` is started outside Steam: check before P3 step 4. With its focus spoofed, the hidden ER
  may also read the pad directly, so one press could arrive twice (directly and via Skyrim). Decide in P3/P4: block ER's own pad reading,
  or let ER read the pad itself (lower latency) and use Skyrim only for gating.
- Can ER be kept running at full rate while hidden/unfocused? What's the cost when both games run at once?
- What's the best hook point in Skyrim to cancel incoming player damage and redirect it?
- How do we take Skyrim's "hit happened" and get ER's damage formula result without a real ER target? Options: a dummy enemy in the arena
  with matching defenses, or calling ER's damage calculation directly through eldenring-rs.
- Mapping Skyrim weapons to ER weapons (EquipParamWeapon IDs). This needs a mapping table in `config/`.
