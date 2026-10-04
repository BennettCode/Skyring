# P3 plan: one value across, then the loop

## Context
P2 is done (link, heartbeat, fail-safe; commit `47fcb89`). P3 proves the core passthrough loop: dodge pressed in Skyrim → hidden
ER character dodges (ER's own gating and stamina) → ER state (stamina, i-frames) comes back to Skyrim's log.
**ROADMAP P3 accept:** pressing dodge in Skyrim → ER rolls → Skyrim's log shows the stamina drop and the i-frame window, with timestamps.

**User decisions (2026-10-04):**
- **Dodge = Skyrim's Sprint user event.** Its held state is forwarded raw to ER's `sp_move` bit, so ER's own tap = roll / hold = dash
  logic applies. Vanilla sprint still fires until P4 swallows input.
- **Arena = parked save.** The user takes the test character to Roundtable Hold once and quits there. On each run the user presses
  **Continue** on ER's title screen. The plugin logs the block ID and hides the window once the player is in the world. No warp or auto-load in P3.

**Research findings** (eldenring-rs @ 59fbd3b; SkyCraft). These go into `docs/research/elden-ring-input.md` and a new `docs/research/elden-ring-state.md`:
- Inject: `main_player.chr_ins.modules.action_request.action_requests.set_sp_move(true)`. The module updates during `ChrIns_PreBehavior`.
  Our write timing relative to the engine's own fill is **unknown → experiment** (step 2).
- No stick/direction field in that module. With no movement input, `sp_move` = **backstep**. It costs stamina in combat but sets **no i-frame flag**
  (measured, see step 4), so the i-frame part of the accept needs a roll. Steering = virtual analog `MoveForwards/Left/...` via `DLUserInputDeviceImpl::set_virtual_analog_state`. That's step 5 (required for the accept).
- State: `modules.data` (`hp/max_hp/fp/max_fp/stamina/max_stamina` i32). Anim = `modules.time_act.anim_queue[read_idx].anim_id`.
  I-frames = `modules.action_flag.action_modifiers_flags` (`perfect_invincibility` b0, `dodging` b1, `super_armor` b8).
  Poise = `modules.super_armor.sa_durability(_max)`. Transform = `modules.physics.position/orientation`. Map = `PlayerIns.current_block_id`.
- Focus: `DLUserInputManagerImpl::instance_mut().is_game_window_focused` and `FD4PadManager.is_back_ground_window` ("disables inputs while
  true"). Window = `CSWindowImp.window_handle`.
- Skyrim: per-frame hook = `RE::VTABLE_PlayerCharacter[0]` `write_vfunc(0xAD)` (`Actor::Update`, SkyCraft `Game.cpp:1035`).
  Input = `BSTEventSink<InputEvent*>` on `BSInputDeviceManager`, `button->QUserEvent() == UserEvents::sprint`. Seqlock pattern = SkyCraft `Link.cpp:138`.

## Steps (each one builds, gets tested, then commit + push)

### 1. ER keeps running hidden (ER only, no protocol change)
- `er-plugin/src/game.rs` (new): helpers for reading `main_player` modules, used by the later steps too.
- `er-plugin/src/window.rs` (new): every frame (FrameBegin) force `is_game_window_focused = true` and `is_back_ground_window = false`.
  Once the main player is in the world, `ShowWindow(hwnd, SW_HIDE)` once. When the player leaves the world, show the window again so the
  title screen stays usable. Hiding can be turned off with `SKYRIMXER_ER_VISIBLE=1` (env var, passed through by `dev.ps1 -ErVisible`).
  If me3 doesn't pass env vars through, use a flag file next to the DLL.
- Log every 5 s: fps (FrameBegin count), `block_id`, stamina/hp/fp, anim_id, position. Log `in world at mXX_XX_XX_XX` once.
- **Test (agent + user):** `dev.ps1 -Game eldenring`. The user presses Continue. The agent puts another window in the foreground and checks that fps stays ~60
  and the frames keep advancing while ER is hidden. This fills in RECON §B (hidden) and the arena block ID.

### 2. Injected dodge works in ER (ER only)
- `er-plugin/src/actions.rs` (new): self-test mode (`SKYRIMXER_ER_SELFTEST=dodge`) that pulses `sp_move` for 3 frames every 4 s.
- First try writing in a `ChrIns_PreBehavior` task. Log stamina before, then for 2 s log changes of anim_id / iframe bits / stamina with
  frame + timestamp, plus `new_action_presses` readback. If the bit gets overwritten (no anim change), try in order: an earlier group
  (`ChrIns_CalcUpdateInfo` / `ChrIns_AILogic`), then virtual digital `Backstep` input at `PadStep`. Two failed attempts → stuck protocol (CLAUDE §10).
- **Result (session 3):** works while focused: hold `Backstep` + `BackstepTapped` on the press frame only (a real tap raises both; the
  release then becomes request bit 16 `backstep`). Hidden: PadStep re-sets `is_back_ground_window` every frame, so the focus spoof also
  runs at WorldChrMan_Prepare. ✔ 7/8 pulses backstep with ER hidden (miss = during spawn-in).
- **Test:** ER only, hidden. The log shows a backstep anim, `dodging`/`perfect_invincibility` on→off, and stamina dropping.

### 3. Protocol v2: seqlock slots (no game needed)
- `protocol/schema/messages.toml`: version 2, `Local\SkyrimXER_v2`. New region blocks after the ring headers:
  `SLOT_INPUT` @0x200 `InputState` (sky→er), `SLOT_PLAYER` @0x300 `PlayerState` (er→sky). The ring data offsets stay as they are.
  - `InputState`: `seq u32, _pad u32, frame u64, time_ms u64, buttons u32` (our own `Button` enum bits: Dodge=0, later Attack/Heavy/…),
    `_pad u32, move_x f32, move_y f32, cam_yaw f32, _pad f32`.
  - `PlayerState`: `seq u32, flags u32` (`PlayerFlag`: InWorld, IFrame, Dodging, HyperArmor, PoiseBroken), `frame u64, time_ms u64,
    hp/max_hp/fp/max_fp/stamina/max_stamina i32, anim_id i32, block_id i32, poise f32, poise_max f32, pos f32[3], yaw f32` (+ explicit padding).
  - Seqlock convention (DESIGN §4): `seq` odd = writing. Writer: seq+1 (relaxed), release fence, copy the body, seq+2 (release).
    Reader: load acquire, retry while odd, copy, acquire fence, accept if seq is unchanged (≤64 tries). Data counts as **stale** if `time_ms` is older than 250 ms or the link isn't connected.
- `tools/protogen`: no new syntax needed (slot = struct block). Add a check that a struct used in a `SLOT_*` block starts with `seq: u32`.
- `protocol/src/slot.rs` (new): `SlotWriter<T: Plain>` / `SlotReader<T>` over a region offset. Tests include a concurrent writer/reader torn-read test.
- `skse/src/bridge/Slot.h` (new): C++ mirror. Selftest in `skse/tests/link_test.cpp`, plus a cross-process slot round trip in `tests/run-tests.ps1`.
- Link (`protocol/src/link.rs`, `skse/src/bridge/Link.cpp`): once joined, publish the region base pointer (atomic) and keep the view mapped for the
  rest of the process, so game threads can use slots without taking the link mutex. Add a `connected` atomic flag for the game threads.
- `tools/fake-peer`: `er` writes a synthetic PlayerState (stamina −20 / 1 s of IFrame on each Dodge edge, regen afterwards). `skyrim` pulses Dodge every 4 s.
- `docs/DESIGN.md` §4: v2 layout table.
- **Result (2026-10-04):** ✔ as specified. Extras: `LinkShared` (Rust `Arc`, C++ member of the leaked Link), `SLOT_STALE_MS`/`SLOT_READ_TRIES`
  constants, `Button`/`PlayerFlag` values are bit indices, body copied as u32 atomics. Fake peers run a link thread + a 16 ms frame loop.
  `run-tests.ps1` interop A now also checks C++ Dodge → Rust ER dodge → C++ sees stamina 100→80 + IFrame on/off, and stale once after the crash.

### 4. Wire the loop (both plugins)
- **Skyrim**: `skse/src/hooks/PlayerUpdate.cpp` (new, own file with a comment, CLAUDE §7): vtable hook 0xAD on `VTABLE_PlayerCharacter`. Every frame:
  frame counter (also goes into Heartbeat instead of `0`), write InputState, read PlayerState. Log on **edges** (IFrame on/off, anim_id change,
  stamina change ≥1, InWorld change) plus a 1-in-30 sample line, and frame-time p50/p95/p99 every 5 s.
  `skse/src/bridge/Input.cpp` (new): input sink. Sprint held → `Button::Dodge`. Install both on kDataLoaded.
  `SKSE::Init` needs a trampoline? No: a vtable write needs none.
- **ER**: the `ChrIns_PreBehavior` task (or whichever group step 2 settled on) reads InputState. If it's fresh and connected, OR the mapped bits into
  `action_requests`. Stale or lost data = inject nothing (fail-safe). A `ChrIns_PostPhysics` task writes PlayerState every frame. ER logs
  the Dodge edge + stamina before/after.
- **Test order:** `fake-peer er` + real Skyrim, then real ER + `fake-peer skyrim`, then both games (the user presses Continue in ER, loads a Skyrim test save,
  taps Sprint 3× and holds it once). The agent merges the logs by timestamp. **The P3 accept is met here.**

- **Result (2026-10-04):** the loop works in both games (keyboard Sprint in Skyrim; Skyrim gets no DualSense without Steam Input, so the
  controller is deferred). Tap → ER backstep (27010), seen in Skyrim's log ~170 ms after the press; stale input releases the key.
  Not shown yet: stamina stays 101 and no i-frame flag is set during backsteps out of combat (`docs/research/elden-ring-state.md`).
- **I-frames (2026-10-04, attempt 3):** `PlayerFlag::IFrame` = `action_modifiers_flags` bit 1 (FLAG_AS_DODGING) or an invincibility bit.
  Rolls set bit 1 for 26–27 frames from the roll's first frame and no hit landed while it was set (46 hits logged in a play test). Backsteps set
  nothing and get hit, so Skyrim only sees `IFrame` once step 5 makes the hidden ER character roll.

### 5. Roll direction + coordinate test (finishes the ROADMAP P3 list)
- Skyrim forward/strafe user events + gamepad move stick → `move_x/move_y`. ER: `set_virtual_analog_state` on MoveForwards/… at `PadStep`
  (the experiment: does ER then walk/roll in that direction?).
- **Result, roll direction (2026-10-04):** Skyrim's Forward/Back/Strafe user events → `move_x/move_y` → ER `set_virtual_analog_state` on the
  MoveForwards/Backwards/Left/Right slots (66/65/64/63) in WorldChrMan_Prepare, every frame while held. **Forwards/Right take +1,
  Backwards/Left take −1** (a positive value there is ignored). ER self-test: 7/7 short taps in all four directions roll (27110, dodge flag
  27 frames); 30-frame holds dash. Both games: 8/8 Sprint + direction → roll, Skyrim `IFrame on` 34–51 ms after the release, off ~450 ms
  later, HUD "i-frames yes"; 2/2 Sprint alone → backstep, "i-frames no". **P3 accept met.**
- **Result, coordinate test (2026-10-04):** one both-games walk (W/D/S/A, `[coords]` samples on both sides) → conventions in DESIGN §6,
  `protocol/src/coords.rs` + `skse/src/bridge/Coords.h` with the measured segments as unit-test fixtures. **P3 done.**
- Coordinate test per RECON §D: the same Skyrim input drives both. Walk N/E, compare position deltas + yaw in both logs → conversion in
  `protocol/src/coords.rs` + unit test + C++ mirror, result in DESIGN §6.

## Files touched (main)
`protocol/schema/messages.toml`, `protocol/generated/*` (regen), `protocol/src/{slot,link,coords}.rs`, `tools/protogen/src/main.rs`,
`tools/fake-peer/src/main.rs`, `er-plugin/src/{lib,bridge,game,window,actions}.rs`, `er-plugin/Cargo.toml` (windows features for ShowWindow),
`skse/src/{main.cpp,bridge/Bridge.cpp,bridge/Link.*,bridge/Slot.h,bridge/Input.cpp,hooks/PlayerUpdate.cpp}`, `skse/CMakeLists.txt`,
`skse/tests/link_test.cpp`, `tests/run-tests.ps1`, `tools/dev.ps1` (`-ErVisible`, `-ErSelfTest`), docs (DESIGN, ROADMAP, RECON, research,
STATUS, MODLOG), THIRD-PARTY-NOTICES (SkyCraft seqlock + hook pattern credit).

## Verification
- `tests/run-tests.ps1` green (slot layout + torn-read + cross-process) before any protocol commit. `cargo run -p protogen -- --check` clean.
- Each in-game step goes through `tools/dev.ps1` (save backup first, me3 offline). The agent reads the `[core]`/`[input]`/`[state]` lines.
- Final accept: the merged log shows `[SKY] dodge down t0` → `[ER] sp_move injected, stamina A` → `[ER] anim X, iframe on t1..t2` →
  `[SKY] PlayerState stamina A→B, iframe on/off` with timestamps within ~2 frames.
- Fail-safe check: kill ER mid-hold → Skyrim marks PlayerState stale and logs it once. ER with stale input injects nothing.

Planned now, decided before step 2: if step 2 fails twice, write a handoff into claudeprogress.md and continue in a fresh chat.
