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

## Moving the player with collision: the character controller's velocity (LOCO-PLAN stage A)
- **Used:** `bhkCharProxyController::SetLinearVelocityImpl` = bhkCharacterController vfunc **0x07**, hooked in
  `RE::VTABLE_bhkCharProxyController[1]` = AE **240560** (`skse/src/hooks/ControllerVelocity.cpp`). The player's controller is a
  bhkCharProxyController. Its bhkCharacterController part (what `GetCharController()` returns) uses the class's **second** vtable,
  because `hkpCharacterProxyListener` comes first; `[0]` (240558) is the listener's.
- Skyrim calls it once per frame for the player, from a Havok worker thread, with the locomotion's velocity (Havok units/s =
  Skyrim units/s × `bhkWorld::GetWorldScale()`). Replacing x/y there (keeping z) moves the player at exactly that speed, through
  Skyrim's collision.
- Measured 2026-10-05 (5 dodges, all directions):
  - 0 stalled frames, 98–100 % of ER's distance.
  - The realized speed is ER's interpolated speed one frame later, so its frame-to-frame change is ER's own (18–35 % of peak).
  - Rolling into a wall: 0.21 m of 3.20 m, then the player slides along it.
- **Didn't work (same session):**
  - `Actor::ApplyCurrent(time, velocity)` (Actor vfunc 0x9D) is refused on alternate frames even with the controller's
    `velocityTime` zeroed first: 12–15 stalls in a 44-frame roll.
  - Writing the controller's linear velocity (`SetLinearVelocityImpl` from PlayerCharacter::Update) or `velocityMod` (+0xB0: the
    locomotion's wanted velocity in the body frame, x right / y forward, Havok units) after PlayerCharacter::Update doesn't stick.
    Skyrim sets both again before the Havok step.
  - Skyrim's own run (moveInputVec zeroed) keeps `velocityMod` at run speed for ~20 frames into a roll.
- Confidence: verified by test (2026-10-05).

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

## Posing the player's skeleton (POSE-PLAN step 2, 2026-10-04)
- **Write point:** right after `PlayerCharacter::Update` (vfunc 0xAD, our existing hook), on the main thread.
  - Set `NiAVObject::local.rotate`, then call `UpdateDownwardPass(NiUpdateData{}, 0)` on that node. Both are virtual, so no new Address Library ids.
  - Test: F7 turns the pelvis 45° and the legs visibly turn.
  - The animation re-poses the skeleton before the next Update, so the write must be repeated every frame. The trace shows our value always gone by the next pre-Update.
- **`UpdateAnimation` (vfunc 0x7D)** is called once per frame for the player (595 times in 10 s), but on a **worker thread**. Not used: writing there would race the main thread.
- **Skeleton:** the third-person 3D is `Skeleton.nif` (`Get3D(false)`). Bones, child<parent:
  | Chain | Bones |
  |---|---|
  | Root | NPC Root [Root] < NPC; NPC COM [COM ] < Root |
  | Legs | NPC Pelvis [Pelv] < COM; NPC L/R Thigh [LThg]/[RThg] < Pelvis; NPC L/R Calf [LClf]/[RClf]; NPC L/R Foot [Lft ]/[Rft ] |
  | Spine | NPC Spine [Spn0] < **COM** (not the pelvis); Spine1 [Spn1]; Spine2 [Spn2]; NPC Neck [Neck]; NPC Head [Head] |
  | Left arm | NPC L Clavicle [LClv] < Spine2; NPC L UpperArm [LUar]; NPC L Forearm [LLar]; NPC L Hand [LHnd] |
  | Right arm | NPC R Clavicle [RClv] < Spine2; NPC R UpperArm [RUar]; NPC R Forearm [RLar]; NPC R Hand [RHnd] |
  | Helpers | upper-arm twist, pauldrons, skirt bones, WEAPON/SHIELD/QUIVER, AnimObjectA/B/L/R |

  Note the trailing spaces in `[COM ]`, `[Lft ]` and `[Rft ]`. The shape matches ER's skeleton: legs under the pelvis, spine under the root (`docs/research/elden-ring-pose.md`).
- **Bind pose source:** the body's skinned geometry (`BSGeometry` → `skinInstance` → `NiSkinData::boneData[i].skinToBone`, bones in `skinInstance->bones`).
  - The default body has 26 bones, with the pelvis at index 0.
  - Pelvis skinToBone: rotation = identity, t = (0, 0, −68.91) Skyrim units.
  - Pelvis world scale is 1.03 (race height).

## Dev auto-load (2026-10-04)
- `skse/src/bridge/AutoLoad.cpp`: `BSTEventSink<MenuOpenCloseEvent>` on `RE::UI` (singleton AE 400327). When "Main Menu" opens, an SKSE task
  calls `BGSSaveLoadManager` (singleton AE 403340) `Load(name, false)` (AE 35757). All ids checked with `tools/addrlib-check.ps1`.
- `LoadMostRecentSaveGame` (AE 35766) returned **false** when called right as the main menu opened: the menu hadn't listed the saves yet.
  So `launch.ps1 -AutoLoad` passes the newest `.ess` file's name in `SKYRIMXER_AUTOLOAD` instead. Load time: ~11 s from the menu.
- Input: SendInput with scan codes reaches Skyrim's input sink (`[input] user event 'Forward'/'Sprint'` from the key script).
- Focus: under Chrome Remote Desktop, SetForegroundWindow, AttachThreadInput and the Alt trick all fail (the foreground belongs to
  `remoting_desktop`); `SwitchToThisWindow` works.

## Applying the ER pose (POSE-PLAN step 5, 2026-10-04)
- `skse/src/bridge/Pose.cpp`, called from `bridge::OnFrame` after `movement::Update`, i.e. right after PlayerCharacter::Update.
  Per bone: world = R_root · Yaw · delta · fit · hostBind. Locals are computed through the parents' world rotations, slerped with
  the animation's local (5 frames in/out). The pelvis offset moves **NPC COM** (parent of both Pelvis and Spine; moving the pelvis
  alone stretched the body at the waist), then one `UpdateDownwardPass` on COM. No new Address Library ids.
- **Host bind:** inverse `skinToBone` from all 25 skinned geometries, moved into the 3D root's space. The bind is an A-pose:
  hands at (±28.9, 1.8, 72.8) units, pelvis (0, 0, 68.9), head (0, −1.6, 120.3). **No mesh is skinned to `NPC Neck`**, so its bind
  is derived: the parent's bind × today's local.
- **Fits** (Skyrim bind segment → ER bind segment, degrees): clavicles 37, upper arms 21, forearms/hands 26, Spine2 14, legs 5–7.
  Pelvis → Spine gave 54° (Skyrim's spine sits just above and behind the pelvis), so the pelvis aligns its hip axis (L → R thigh) instead (0°).
- **Check line** `[pose] check: segment error deg`: posed segment vs ER's segment, 0° for spine, neck, arms and legs.
  Hands and feet aren't checked: `Finger20`/`Toe0` have no skin bind, so they reuse the forearm/calf fit.
- **Facing:** `movement::RollHeading()` (ER's forward is laid along it), else the body heading. Heading grows = turning right = −Z rotation.
  Verified: a right roll (D) turns the body +90° and it tumbles that way.
- **NPC COM turns with the yaw** (2026-10-05). COM, the parent of the pelvis and the spine, kept the rotation Skyrim's animation gave it,
  which faces the actor's heading. During locomotion the actor's heading is often 60–180° from the body's facing, so the spine's root (which
  sits behind the pelvis) ended up beside or in front of the hips. `[body]` probe: running forward leaned 4° more than ER, running back
  toward the camera 11° more. Fixed by turning COM by the yaw; the pelvis position is now predicted from COM's new rotation. After the fix,
  hip/chest/lean match ER within 4° (run, sprint, strafe, back, weapon drawn).
- **Spine deltas are conjugated by their bind fit** (2026-10-05): Spine/Spine1/Spine2 use `fit⁻¹·delta·fit` (fits 2°, 6°, 14°).
  Applied unchanged, ER's sprint chest twist (about ER's own spine axis) swung Skyrim's Spine2 round in a cone, because it rests 14° further
  forward than ER's. Frame-matched `[bodydump]` probe, 330 sprint frames: Spine2→neck swayed ±22° sideways against ER's ±9°, and the head
  ±15 cm against ±7. Conjugated: ±11.5° against ±9.4°, trunk error mean 5.2° → 2.5°, limbs unchanged ≤ 1°. At rest it changes nothing
  (a plain fit on shape bones stretched the chest, 2026-10-04). Remaining constant differences are bind shape only: clavicles 37°,
  Spine2→neck 14°; Skyrim's upper torso is ~40% longer, so the same angle moves the head further.
- **`[body]` line** (both sides, every 120 moving frames): face/hip/chest yaw and forward/side lean relative to the travel direction, mean/largest
  in degrees (`Pose.cpp` MeasureBody, `er-plugin/src/body.rs`). The two sides compare without matching clocks.
- **Cost:** ≤ 0.14 ms per frame while posing (hook p99 ~0.1 ms, 0.01 ms idle); frame time unchanged (17 ms p99).
- **Polish later:** prefer the body mesh's bind (today the first skinned mesh wins, so armour with another bind can tilt a bone), finger and toe bones, foot planting/IK, smoothing between ER frames when Skyrim runs above 60 fps,
  ending the pose at ER's move-cancel window instead of the end of the animation (101 frames).

## ApplyCurrent is uneven frame to frame (LOCO-PLAN stage A, 2026-10-05)
- `ApplyCurrent(velocityTime, v)` returns **false and ignores the call while a previous current is still running**. Probe, roll, 60 frames:
  - 1-frame currents: accepted on alternate frames, so the motion comes in bursts (the playtest judder).
  - 2-frame currents: frame 1 accepted, frames 2-4 refused. Stalls 23 (frames with no movement while ER moves), biggest frame-to-frame
    change 45% of peak, 89% of ER's distance.
  - 0.75-frame currents with the velocity scaled up: stalls 25-27, spikes up to 89% of peak, 94% of the distance.
  - The drawn 3D root stalls exactly like `GetPosition()`, so this is real, not a measuring artefact.
- Both references move their player with `SetPosition` each frame (controller velocity zeroed): SkyCraft `Game.cpp:790`, FalloutCraft
  `fo_game.cpp:786`. They rely on the hidden game's collision, which we can't (ER's world isn't Skyrim's).

## Melee hits on the player (P4 step 7, 2026-10-05)
- AE 38627 = the melee HitFrame handler; it builds a HitData (43995) and at **+0x4A8** does `call 38586` (apply the hit to the victim:
  damage, block, stagger, hit reaction, TESHitEvent, kill credit). Same site SkyCraft hooks. Byte-checked at load (`E8` + target), both
  ids pass `addrlib-check`. `write_call<5>` there needs `SKSE::AllocTrampoline` (64 B).
- Skipping the call drops the whole hit: verified with wolves, no damage and no stagger inside i-frames.
- Not covered: projectiles and spells (they don't go through 38627). Find their apply path in P5.
- SkyCraft hooks the same call: never run both plugins at once.

## Fingers and toes are not nodes (2026-10-05)
- The player's third-person tree has 51 nodes and no finger or toe nodes. The skinned hands (36 bones), feet (6) and body (24) still skin
  fingers and toes, through `NiSkinInstance::boneWorldTransforms` entries that point **outside the tree**: loose world transforms the
  animation writes directly, 0x80 apart, sitting at the hands and feet.
- They are not `NiAVObject::world` members: reading a node out of one crashed (access violation).
- Posing the hands and feet without them left the fingers and toes where Skyrim's animation put them, so the hands, wrists and feet
  stretched during rolls.
- **Fix** (`Pose.cpp` Carry): after posing, each such transform is moved rigidly with its nearest posed bone (posed world × animation
  world⁻¹). The pointers are read from the live skin instances every frame. Verified on a contact sheet: hands and feet follow
  through the whole tumble.
