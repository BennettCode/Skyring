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
