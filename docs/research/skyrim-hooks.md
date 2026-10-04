# Skyrim hooks and Address Library IDs

All IDs are AE ids for SkyrimSE.exe 1.7.104, checked against the installed `versionlib-1-7-104-0.bin` with `tools/addrlib-check.ps1`
before use (a missing id makes CommonLib abort the game at load).

## Address Library database format
- Game/version: SkyrimSE 1.7.104, Address Library v13 (All in One).
- `versionlib-1-7-104-0.bin` is **format 5**: a 96-byte header (format i32, version u32[4], name char[64], pointer size i32, data format i32,
  offset count i32), then a dense u32 table where entry `id` is the offset (0 = no such id). Older AE files (1.6.x) use the
  compressed format 2, which `addrlib-check.ps1` doesn't read.
- Confidence: verified by test (the checker matches CommonLib's `IDDB::load_v5` behaviour; a made-up id reads as missing).

## PlayerCharacter::Update (per-frame hook)
- Location: `RE::VTABLE_PlayerCharacter[0]` = AE id **208040** (offset 0x19296c0), vfunc **0xAD** (`Actor::Update(float delta)`, CommonLib `RE/A/Actor.h`).
- What it does: runs once per frame on the main thread while the game isn't paused. Menus that pause the game stop it (so our InputState
  goes stale, which is the intended fail-safe). Not called at the main menu.
- Hook: `skse/src/hooks/PlayerUpdate.cpp`, a vtable write (no trampoline) that calls the original first. Same hook as SkyCraft (`Game.cpp`).
- Confidence: verified by test (2026-10-04: ~60 calls/s, p50 16.8 ms frame time; our work ≈ 2 µs per frame, ~45 µs on frames that log).

## HUD notification
- Location: `RE::SendHUDMessage::ShowHUDMessage` = AE id **52933** (offset 0x991a30), the game's debug notification (top-left text).
- Used by `skse/src/bridge/Hud.cpp` from the main thread (PlayerCharacter::Update). Confidence: verified by test (2026-10-04).

## Input: BSInputDeviceManager event sink
- Location: `BSInputDeviceManager` singleton AE id **402776**, `UserEvents` singleton AE id **402638**.
- What it does: `BSTEventSink<InputEvent*>` gets every input event batch on the main thread. `ButtonEvent::QUserEvent()` compared with
  `UserEvents::sprint` gives the Sprint action independent of the key binding; `IsPressed()` is the held state.
- Observed (vanilla keymap, no ControlMap_Custom.txt): keyboard **Sprint = Left Shift (0x2a)**; Left Alt (0x38) is `Run`.
- **No gamepad events when Skyrim is started by `skse64_loader.exe` outside Steam**: the DualSense needs Steam Input, which only applies to
  Steam launches. A Steam launch option `"...\skse64_loader.exe" %command%` fails: the loader logs `too many free args (...SkyrimSELauncher.exe)`
  and never starts the game. Controller support is still open (options: a `cmd /c start ... & rem %command%` launch option, or a Non-Steam shortcut).
- Confidence: verified by test (2026-10-04).

## SprintHandler::CanProcess (vanilla sprint off)
- Location: `RE::VTABLE_SprintHandler[0]` = AE id **208717** (offset 0x1935208), vfunc **0x1** (`PlayerInputHandler::CanProcess(InputEvent*)`;
  CommonLib's AE 1.7.99 vfunc shift only starts at 0x2).
- What it does: PlayerControls asks each handler whether it takes an event. Returning false for Sprint *presses* stops vanilla sprint;
  our `BSInputDeviceManager` sink still sees the key. Releases are let through so a sprint begun before the swallow still ends.
- Hook: `skse/src/hooks/SprintSwallow.cpp`. Confidence: verified by test (2026-10-04: bridged Shift+W ≈ 366 u/s = run speed, unswallowed
  sprint ≈ 499 u/s).

## Moving the player with collision: Actor::ApplyCurrent (P4 step 4)
- `Actor::ApplyCurrent(velocityTime, hkVector4 velocity)` = Actor vfunc **0x9D** (CommonLib, through the player's own vtable, no id): a
  velocity the character controller applies (Havok units/s = Skyrim units/s × `bhkWorld::GetWorldScale()`), with collision.
- Open loop it realizes an uneven share of the velocity (79–150 % over a roll). Closed loop (steer onto a target that moves with ER,
  lead capped) gives 96–99 %. Confidence: verified by test (2026-10-04).

## MovementHandler::CanProcess, PlayerControls, PlayerCamera
- `RE::VTABLE_MovementHandler[0]` = AE **208715**, vfunc 0x1: refusing events keeps held movement keys from setting
  `PlayerControls::data.moveInputVec` (zeroed by us); held keys send an event every frame, so movement resumes right after.
- `PlayerControls` singleton AE **400864**; `PlayerCamera` singleton AE **400802**, `GetRuntimeData2().yaw` = camera yaw in the same
  convention as `GetAngleZ()`.
- Turning the player (`Actor::SetHeading`, AE 37230) during a roll turns the third-person camera with it: don't.
- SprintHandler only starts sprinting on a fresh press: to start it mid-hold, set the held event's `heldDownSecs` to 0 once.
- Confidence: verified by test (2026-10-04).

## Vanilla sneak roll (research, P4 step 5)
- With the Silent Roll perk, sneak + sprint plays a forward roll. Graph notifications: `tailSneakLocomotion` → `tailSprint` →
  `SprintStop` (~0.48 s). Forcing it (SneakStart, then SprintStart, actorState sneaking/sprinting bits) starts it, but it ends after
  ~8 frames when the move input is zero. Confidence: likely (one session, 2026-10-04).
- **Pressing Sneak like the player:** `RE::ButtonEvent::Create(kKeyboard, UserEvents::sneak, 0x1D, 1, 0)` handed to
  `PlayerControls::sneakHandler` (`CanProcess` vfunc 0x1, then `ProcessButton` vfunc 0x4, through the object's vtable), then `RE::free`.
  Toggles sneak exactly like the key (HUD eye, stealth state); `IsSneaking()` changes the same frame. Two presses within a frame or two
  don't both take. Ids used (all present, addrlib-check): ButtonEvent vtable 208708, UserEvents 402638, heap 68088/68115-68117/11141/36091.
  Confidence: verified by test (2026-10-04, ~40 rolls).
- **Perks:** `Actor::HasPerk` AE 37698 (checked), `AddPerk`/`RemovePerk` = Actor vfuncs 0xFB/0xFC; Silent Roll = 00105F23.
- **Turning the player in third person doesn't stick:** `Actor::SetHeading` (AE 37230, checked) every frame plus
  `ThirdPersonState.freeRotationEnabled/freeRotation.x` set to the difference: the logged heading stays on the camera yaw the next frame
  (the camera state re-aligns the body). Confidence: verified by test (2026-10-04, 7 rolls). Turning the body needs a camera-state hook
  (how True Directional Movement does it) or a different animation source.
