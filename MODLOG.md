# MODLOG

Newest first. One entry per session or verified step: **what changed · how it was tested · result · follow-ups**.

---

## 2026-10-06: Swords point the Elden Ring way; bows ride in the right hand; shots survive menus (protocol v13)
- **User:** "a normal Skyrim sword looks like a dagger in the animation; running with the bow is weird; I can't shoot arrows."
- **Measured:** ER's `R_Weapon` sits at a fixed angle in the hand (constant hand-local axes through idle, walk, most of a swing) with
  the blade on its +Y; Skyrim's `WEAPON` node (held at its bind local) pointed the sword up where ER's points forward/down. ER carries a
  bow sideways in the **right** hand and moves it to the left only for draw/hold/release (x36010/x36020/x36000); Skyrim kept it in the left.
  Bow shots failed after menus: stale input made ER put its own weapon/ammo back, and the first R1 after re-equipping hit an empty quiver.
- **Changed:** protocol v13: PoseState `blade` (L/R_Weapon +Y) + PoseFlag `BowLeft`, PoseBind `thumb`. Skyrim `Pose.cpp`: AimWeapon
  turns `WEAPON` so the weapon mesh's blade (bound centre) follows ER's; CarryBow moves `SHIELD` (the bow) to the right hand's grip,
  upper limb along ER's, except while BowLeft; hands fit two landmarks (middle finger + thumb, from the Havok skeleton: a 9 deg roll fix,
  not the cause). ER `stance.rs`: stale input while connected keeps the mapped weapon/ammo (Skyrim paused in a menu).
- **Tested:** run-tests green (FrameFit test added). Contact sheets, both games: steel sword idle/walk/R1 point forward and swing like
  ER's sheets (before: blade up); hunting bow idle/run in the right hand, draw in the left, back to the right after the shot, arrow
  launched. Earlier session: 4/4 shots including one right after the Tween menu. Not tested: daggers/two-handers sheet by sheet,
  crossbows, shields and left-hand weapons (unchanged).

## 2026-10-05: Animation recheck: the bow sits in the hand; ER ammo never runs out
- **User:** "the bow doesn't work properly" (wrong or odd animation). Side-view screenshots (virtual right stick to orbit): the body
  matched ER's full draw, but the bow hung at the right hip.
- **Cause:** `Pose.cpp` Carry treated the bow's own skinned bones as loose transforms (its tree set was built before the bow was equipped)
  and moved them a second time with the nearest bone of Skyrim's animation. **Fix:** the tree set is rebuilt every frame.
- **Ruled out by measurement:** ER aiming up (camera pitch 0 at full draw) and a late aim pass (arm elevation the same in three task
  groups). ER's ammo is kept at its count while we use it: 26 bridged shots, no empty quiver.
- **Tested:** run-tests green; bow side view after the fix = ER's pose (bow upright in the left hand, right hand at the face); sword
  contact sheet (walk, light, heavy): sword in hand, body on the ground. ER was dismounted by the user (it had loaded on Torrent).

## 2026-10-05: Elden Ring stays off your pad in Skyrim menus; no more "horse" (Torrent) on the Skyrim body
- **User report:** "with a sword I go through the map and it's like I'm on a horse; the bow doesn't work properly".
- **Cause (logs):** ER blanked the physical DualSense only while Skyrim's input was fresh. In Skyrim's inventory (paused, stale input) ER read
  the pad for 4 s and a menu press summoned Torrent: riding anims 100000/101004/120100, pose pelvis 0.84 m down (riding posture), ER's
  position (and so Skyrim's) followed the horse through the terrain. The ER save then loaded mounted, so later bow shots were mounted
  shots (44039700) and every swing looked like riding.
- **Fixed:** ER blanks the pad whenever Skyrim is connected (`remote.rs`); while ER is on Torrent (`CSChrRideModule.is_mounted/
  is_mounting`, `game::is_riding`) its pose isn't streamed and its movement isn't passed to Skyrim (`pose_stream.rs`, `park.rs`), with a
  warning. ER keeps its ammo count while we use it (`stance.rs`; each ER shot used one of its 20 arrows).
- **Tested:** run-tests green; both games, Skyrim console open 5 s: ER log stays "physical pad blanked"; mounted ER: "on Torrent ...
  ignored" warnings, Skyrim not moved. **Not solved:** dismounting from code (Event Action key, E, X: no effect; action_requests writes
  are rebuilt by the engine); the user dismounts once by hand. Ammo top-up and the bow animation recheck are pending (needs ER on foot).

## 2026-10-05: Bows draw and shoot with Elden Ring's animation, protocol v12
- **User request:** "Bows also need an animation" (a Skyrim bow made ER swing its own melee weapon, no arrow).
- **Probe:** ER fires only owned ammo equipped the full way (param id + gaitem handle + inventory index + equip entry; a param id alone
  plays the empty-quiver gesture 50050). Release = anim x36000 (draw x36010, hold x36020). `docs/research/elden-ring-combat.md`.
- **Changed:** protocol v12 (`er_ammo`, AttackKind Shot); WeaponMap bow/crossbow + ammo; ER `stance.rs` equip_ammo/restore, `attack.rs`
  Shot at release; Skyrim `Combat.cpp` Shoot (`Projectile::Launch` AE 44108 with the equipped bow and ammo along the camera's aim, one
  arrow removed). Research switch `-ErAmmo`, fake-peer `--hold-ms`.
- **Tested:** run-tests green; ER alone (visible): draw/hold/release on screen; both games, console hunting bow + iron arrows, virtual pad
  hold-release x4: four ER releases → four Skyrim arrows launched (log), contact sheet shows ER's bow draw on the Skyrim body.
  Not tested: crossbows, arrows actually hitting a target, aim feel.

## 2026-10-05: Your health is your Elden Ring HP, protocol v11 (P5 step 3)
- **Changed:** protocol v11 (InputState `hurt_total` f64 running share + `respawn_seq`; PlayerFlag `Downed`). Skyrim `bridge/Health.cpp`
  (adapted from SkyCraft BridgePlayerDamage/HitSink/KillPlayer, MIT): every health change refunded and forwarded as a share of max
  health, bar = ER's HP share, ER i-frames block any damage, Downed → kill, save load → respawn. ER `er-plugin/src/health.rs`: share →
  ER HP (held at 1, Downed), sub-HP remainder carried, baseline kept across Skyrim pauses (first version lost damage taken in the console).
- **Tested:** run-tests green (+ ER unit test); both games: wolf bites 0.5–0.8% → ER 1450 → 1443 → 1431…; console damage 100 (25%) →
  ER 1088, heal 50 → 1269, bar 300 → 350/400; fatal damage → Skyrim death (the bool essential flag doesn't stop it) → reload → ER refilled
  to 1450. Not tested: arrows/spells during a roll (i-frame block for non-melee), user feel.

## 2026-10-05: Your Skyrim weapon picks the Elden Ring weapon, protocol v10 (P5 step 2)
- **Probe:** an ER weapon id written into the right-hand ChrAsm slots (both copies) switches the moveset and attack rating, no equip call
  (`-ErWeapon` research switch; fake-peer `--stance` / `--weapon`). The per-weapon probe sessions first failed to build (a v10 edit landed
  mid-probe); redone in one session with live swaps.
- **Changed:** protocol v10 InputState `er_weapon`; Skyrim `bridge/WeaponMap.{h,cpp}` (kind + material tier → ER id + level,
  `SkyrimXER.ini [Weapons]`); ER `stance.rs` writes/restores it (slot choice from the save's ids: picking from our own write flipped the
  slot every frame).
- **Tested:** run-tests green (weapon cases); fake Skyrim, 7 ER classes + a +21 sword; both games, console-equipped iron dagger, steel
  sword (+3), iron war axe, mace, greatsword, battleaxe, warhammer, daedric sword (+21): each mapped, distinct moveset (anim groups
  20/23/30/33/25/32/35), AR 102–390; contact sheets of a sword and a warhammer swing look different. User feel test pending.

## 2026-10-05: Hit sound for Elden Ring swings
- **User report:** hits land and look right, but make no sound. `PlayImpactEffect` only spawns the visuals.
- **Fix:** `Combat.cpp` PlayHitSound plays the weapon's impact sound for the target race's blood impact material (fallback: any of the
  weapon's impacts) at the hit node. **Tested:** virtual-pad swing on a wolf, sound handle played (log); user listening test pending.

## 2026-10-05: Elden Ring swings hit Skyrim enemies, protocol v9 (P5 step 1)
- **Probe (stage A):** ER's hit window found in memory: `action_modifiers_flags` bit 9/10 + action_flag +0x1d0, 2-3 frames mid-sweep in
  every swing (`docs/research/elden-ring-combat.md`). The dump tool (`-ErDump`) now records every frame of attack anims plus three untyped
  regions (VirtualQuery-checked); a first version misread `modules` (an OwnedPtr) and crashed ER once.
- **ER:** `er-plugin/src/attack.rs`: hit window, attack rating from ER's params and stats, motion value by attack kind (one-/two-handed
  light 300xx/320xx, heavy 305xx/325xx, skill 40xxx), one swing per animation play. **Protocol v9:** PlayerState attack fields + AttackActive.
- **Skyrim:** `bridge/Combat.cpp` (targets in reach in front of the shown body, poise build-up → stagger, SkyCraft's HitData → 38586
  apply, fallback DoDamage), `bridge/CombatMath.h` (ER defense curve, level defense, armour absorption, HP scaling), `SkyrimXER.ini`
  `[Combat] fDamageScale` (deployed with the DLL). XInputGetCapabilities hooked too: Skyrim never polled a pad it didn't find at startup
  (the dev virtual pad failed with the DualSense unplugged).
- **Tested:** run-tests green (5 damage-model cases); ER alone + fake-peer: one window per swing (light/heavy/skill); both games, console
  wolves, greatsword two-handed, virtual pad: light hits 22→12→1 HP, a heavy kill, one hit per swing after the two-window fix; AR logged
  (phys 88 fire 75). Not yet: user playtest, AR checked against ER's status screen, bandits/armoured NPCs.

## 2026-10-05: Link no longer deadlocks after an Elden Ring stall
- **Bug:** ER froze 36 s while loading. Skyrim timed it out and, in the same tick, connected on ER's queued Hello. On the next tick it read
  the same ER (same attach#) beating again as a restart, dropped the connection and sent a Hello, which ER (not connected yet) took
  without replying. Skyrim then waited forever (`connected=false` until both games restarted).
- **Fix (both sides, `protocol/src/link.rs` + `skse/src/bridge/Link.cpp`):** the same attach# coming back is not a restart (an existing
  connection is kept, logged "beats again; still connected"); while the peer is alive and no handshake happened, the Hello is re-sent
  every 2 s (`HELLO_RETRY_MS`), so a swallowed Hello heals itself.
- **Tested:** new Rust tests `stall_reconnect_never_deadlocks` (replays the field order; failed on the old code) and
  `hello_is_retried_until_answered`; the same replay in the C++ selftest; `tests/run-tests.ps1` green. Live: ER suspended for 10 s
  (NtSuspendProcess) → reconnected one tick after resume; during load-in both new paths fired ("beats again", "Hello re-sent" → CONNECTED).
- **Seen, not fixed:** ER itself freezes for 2–12 s at times during load-in (its own log goes silent); the link now rides through it.

## 2026-10-05: DualSense with Elden Ring's buttons, protocol v8 (P4 step 8)
- **Changed (Skyrim):** `bridge/Gamepad.cpp` reads the DualSense over HID (USB/Bluetooth); `bridge/PadReport.h` parses reports and holds
  the layouts; `hooks/XInput.cpp` answers Skyrim's `XInputGetState` (IAT, ordinal import) with the gameplay layout (Elden Ring's buttons,
  Skyrim's extras on Cross/Triangle/Square/d-pad/Options/Create) or plain Xbox positions in menus; touchpad opens the map
  (`UIMessageQueue`); L1/R1 also switch Journal tabs; menu triggers all-or-nothing; buttons held across a layout switch are ignored until
  released. `hooks/AttackSwallow.cpp` keeps vanilla mouse attacks off; `Input.cpp` reads the left stick (analog) and mouse attack/guard;
  `Bridge.cpp` forwards ER buttons and draws a sheathed weapon first.
- **Protocol v8:** `Button` += Attack, StrongAttack, Guard, Skill. **ER:** `remote.rs` writes them to ER's virtual keys and, while bridged,
  releases every action key and analog value ER read from the physical pad (`pad::clear_virtual`). A first bulk clear of the bitset
  wrote past it (its `integer_count` looks like bytes, not words); replaced the same session by per-key release.
- **Dev tools:** virtual pad (`bridge/PadScript.*`, `tools/pad-input.ps1`, `dev.ps1 -SkyrimPad`), `fake-peer --buttons`.
- **Tested:** fake-peer → ER played Attack 42030000, StrongAttack 42030500/505 (charge), Guard 42034000, Skill 712040000. User pad tests:
  stick walk/run, Circle roll/sprint, R1 swings; their report "touchpad opens the pause menu, tabs don't switch" traced to Steam Input's
  second XInput copy of the pad being Skyrim's source. After the fix: virtual pad + screenshots (map, Journal tabs R1/R2/L1, Tween), then
  the user's pad (map twice, 15 tab switches, Tween). `tests/run-tests.ps1` green (pad parser/layout/script cases).
- **Not done:** ER attacks deal no damage (P5); Skyrim weapon type isn't mapped to an ER weapon yet; keyboard heavy attack/weapon art;
  rumble; Bluetooth untested. Open bug: link deadlock after a long ER stall.

## 2026-10-05: Roll i-frames cancel NPC melee hits (P4 step 7)
- **Changed:** new `skse/src/hooks/PlayerHit.{h,cpp}`: the `call 38586` (apply hit to victim) at melee handler 38627+0x4A8 is replaced
  after a byte check (adapted from SkyCraft `Combat.cpp`, MIT). On the player while ER's i-frames are on, the hit is dropped whole;
  every hit on the player is logged (`[hit] skipped/landed`). `Bridge.cpp` sets the flag every frame from fresh PlayerState only.
  `main.cpp` allocates a 64 B trampoline. `tools/game-input.psm1`: a `type` step and all letters/digits (for console commands).
- **Tested** (both games, wolves via `player.placeatme 23abe N`, key-script rolls): 6 hits skipped, each inside a logged IFrame window;
  12 landed outside; F10 off → 7/7 landed. No stagger on skipped hits; stamina drains as before. `tests/run-tests.ps1` green.
- **Not done:** arrows and spells (other hit paths) still land during rolls → P5. User roll-through feel test pending.

## 2026-10-05: Skyrim's stamina bar shows Elden Ring's stamina (P4 step 6)
- **Changed:** new `skse/src/bridge/Stamina.{h,cpp}`, called every frame from `bridge::OnFrame`. While the link is on and ER is in the
  world, the player's Stamina = Skyrim max × ER stamina/max, set through the damage modifier (`ModActorValue` vfunc, read back to check it took). Max uses
  `GetActorValueMax` (AE id 38469, verified with `addrlib-check`). Link off / stale / dead → untouched, so Skyrim's own stamina returns.
  Pattern adapted from FalloutCraft `fo_combat.cpp:151-188` (MIT; NOTICES row, `licenses/FalloutCraft.txt`, README credit).
- **Tested** (both games, `-ErForceCombat on`, key scripts): 6 rolls + a 4 s sprint, then 16 chained rolls. ER 136 → 8 matched Skyrim
  400 → 24 at every logged step, then refilled to full together. No "didn't take" warning. The HUD bar was visible and shrinking
  (screenshot). F10 off → `mirror off`, on → `mirror on`.
- **Not tested:** the exact 0 case (ER stopped at 8: it refused or spaced the last rolls by its own rules).

## 2026-10-05: Run and sprint pose matches ER (body direction, lean, chest sway)
- **User report:** in the run and sprint, the body direction and lean looked off (weapon sheathed and drawn); then the sprint still looked off.
- **Probe:** a new `[body]` line on both sides (`er-plugin/src/body.rs`, `Pose.cpp` MeasureBody), every 120 moving frames. It gives the
  face, hip and chest yaw plus forward/side lean relative to the travel direction, so the sides compare without matching clocks. A
  bounded frame-matched `[bodydump]` probe compared every bone over 330 sprint frames; it was removed before this commit (method in
  `docs/research/skyrim-hooks.md`).
- **Ruled out by measurement:** ER draws the body with `ChrCtrl.model_matrix`, whose yaw equals the physics yaw (within 1°), with at
  most 4° tilt (`docs/research/elden-ring-pose.md`).
- **Fix 1, NPC COM turns with the yaw:** COM kept Skyrim's animation rotation, which faces the actor's heading (often 60–180° from the
  body's facing), so the spine's root sat beside or in front of the hips. Running leaned 4° more than ER, running back toward the
  camera 11° more. After the fix: hip, chest and lean are within 4° of ER for run, sprint, strafe and back-run, weapon sheathed and drawn.
- **Fix 2, spine deltas conjugated by their bind fit** (`fit⁻¹·delta·fit`, Spine/Spine1/Spine2): ER's sprint chest twist swung
  Skyrim's Spine2 (14° further forward in bind) round in a cone. Upper chest side sway was ±22° against ER's ±9°; now ±11.5°. Trunk error
  mean 5.2° → 2.5°, limbs unchanged (≤ 1°), no change at rest.
- **Tested:** agent key scripts in both games, side-view contact sheets, the frame-matched comparison, then the user's feel test
  (no issue reported). `tests/run-tests.ps1` all passed.

## 2026-10-05: Elden Ring drives walking, running and sprinting (LOCO-PLAN stage B3)
- **Changed:** new `skse/src/bridge/Locomotion.{h,cpp}`. While the bridge is on, ER drives the player:
  - Skyrim's keys go to ER's stick (Caps Lock walk = stick 0.6) with Locomote + `cam_yaw` = look − W. W = Skyrim heading − ER yaw,
    fixed at each start.
  - ER's interpolated displacement is turned by W and goes to the controller velocity (`movement::Follow`, lifted out of Movement.cpp).
  - The pose faces ER yaw + W. `MoveSwallow` refuses Skyrim's movement keys and `moveInputVec` is zeroed. Sprint hold = ER sprint
    (no vanilla kick).
  - Suspended = vanilla, nothing to ER: jump (`bInJumpState`), falling > 0.5 s, swimming, sneaking, mounted, furniture, stagger/knockdown,
    dead, kill move, menu.
  - Yield = vanilla + stage A Sprint-roll: attack, block, spell, weapon draw/sheathe (`IsEquipping`/`IsUnequipping`), a stage A dodge
    in progress.
  - `SKYRIMXER_LOCO=0` = stage A only. `tools/game-input` knows CapsLock.
- **Tested** (both games, auto load-in, key scripts, screenshots):
  - W run 99 %, D strafe 98 %, S run 100 %, sprint 100 % (anim 20210), Caps Lock walk 100 % (20010); 0–1 stalls per 2 s window.
  - Roll from a run: 27110 → 20110 with no hand-back.
  - A wall stops the player: the first sprint ran into a house corner, and the screenshot shows the body against the wall.
  - Jump: vanilla, then back on landing.
  - F10 off/on: stop/start. R draw/sheathe: vanilla for the whole animation (32/20 frames).
  - Mouse turn ~90° standing: the body keeps its facing (screenshot).
  - Frame time p95 17 ms, hook p99 ~100 µs (unchanged). `tests/run-tests.ps1` all passed.
- **Fixed during testing:**
  - A 6-frame "in the air" limit broke a sprint off a porch. Now: jump state, or 30 frames.
  - The pose blinked for 2 frames at each restart (ER's Active flag lags a frame); it's forced Active while running.
- **Review agent** (diff before the commit) found that a roll interrupted by a yield/suspend was picked up half-way by the stage A path.
  Fixed (`movement::Reset(spent)`) and retested: jump mid-roll → no stage A start. Also fixed: override cleared when there's no
  controller, a Sprint hold begun under locomotion never becomes vanilla sprint, midair counter reset.
- **Not done:** the user's feel test. Jumps and other vanilla states show Skyrim's own heading, so the body can flip (seen on a jump
  toward the camera: the jump faced away). Combat stamina drain while sprinting was only measured ER-side (B1). NPCs still see Skyrim's own
  heading (the visible body turns, the actor doesn't). Footstep sounds untested.

## 2026-10-05: protocol v7, ER drives locomotion on request (LOCO-PLAN stage B2)
- **Changed:** protocol v7: new `InputFlag::Locomote`; InputState `cam_yaw` = Skyrim's look as a yaw in ER's world; PlayerState
  `cam_yaw` = ER's camera yaw (filled now). No layout change. ER `remote.rs`, with Locomote: the stick is forwarded every frame and
  turned by `cam_yaw − ER camera yaw`, and Dodge is forwarded for the whole hold (tap = roll, hold = sprint). Without it, the
  dodge-only path is unchanged. `pose_stream.rs`: Active while Locomote is on. `fake-peer skyrim --walk DEG`.
- **Tested:** `tests/run-tests.ps1` all passed. Real ER + `fake-peer skyrim --walk 0` (cam_yaw 0/90/180/−90°, 3 s runs): ER yaw =
  the wanted yaw with 0° error in all 4 directions, run anim 20110, stop 22100, pose Active on/off with Locomote.
- **Not done:** the Skyrim side doesn't send Locomote yet (B3), so in-game behaviour is unchanged.

## 2026-10-05: ER locomotion probe (LOCO-PLAN stage B1)
- **Changed:** `game::camera_yaw()` (ER camera view matrix → yaw in the player's convention). `actions::LocoTest`, a scripted
  stick/Dodge self-test (`dev.ps1 -ErSelfTest walk|sprint`) that logs speed, yaw, camera yaw, anim and stamina.
- **Tested** (ER only, hidden, pinned): stick 0.6 = walk 1.5 m/s, 1.0 = run 3.93 m/s, below ~0.3 nothing. Stick right/back = camera
  +90°/+180° (sign confirmed). Dodge held while running = sprint 5.95 m/s (−10 stamina/s, combat forced); tap = roll back into the
  run. The hidden camera never turns by itself. Real position stayed on the spot. Notes: `docs/research/elden-ring-input.md`.
- **Plan correction:** Skyrim ↔ ER facing uses a world offset fixed when the mode starts, not a per-frame camera offset (that
  would spin the body with the mouse). Next: B2 (protocol v7, ER input in the mode).

## 2026-10-05: smooth roll movement (LOCO-PLAN stage A done)
- **Problem:** the player stalled on 22–27 of ~60 roll frames. `Actor::ApplyCurrent` refuses a new current on alternate frames.
- **Probe** (plan mode, one key script, 5 dodges per mover; numbers in `docs/research/skyrim-hooks.md`):
  1. `velocityTime = 0` before ApplyCurrent: still refused on alternate frames.
  2. Controller linear velocity after PlayerCharacter::Update: overwritten.
  3. `velocityMod` after the update: overwritten.
  4. Hook on `SetLinearVelocityImpl`: works. The player's controller uses the proxy class's second vtable (AE 240560), found by
     looking up its vtable RVA in the Address Library.
- **Changed:** new `skse/src/hooks/ControllerVelocity.cpp` (lock-free override, player only, Skyrim keeps vertical velocity).
  `Movement.cpp` sets ER's velocity there instead of ApplyCurrent. New `[move] smooth` line: ER's own change + per-frame
  realized/ER speeds.
- **Tested** (both games, auto load-in, key script backstep + W/D/S/A rolls):
  - 0 stalls in all 5 dodges, 98–100 % of ER's distance (before: 12–22 stalls, 83–91 %).
  - Speed = ER's speed one frame later.
  - Wall (Whiterun stone wall, 3 left rolls): 2.64 m, then 0.21 m (blocked), then 0.67 m sliding along it. The screenshot shows
    the player on the ground, not in the wall.
- **Not done:** the user hasn't felt it yet. The "< 15 % frame-to-frame change" bar is ER's own roll curve (18–35 %).

## 2026-10-05: ER's weapon stance follows Skyrim's (protocol v6)
- **User:** "my right hand stays up after a roll; backsteps look like I'm holding something". ER's test character two-hands a colossal
  sword, so ER played the weapon-holding variants (backstep 12027010) and the pose copied them.
- **Changed:**
  - Protocol v6: InputState `stance` (enum Stance).
  - Skyrim sends it from IsWeaponDrawn + the right-hand weapon type.
  - `er-plugin/src/stance.rs` StanceSync writes ER's ChrAsm (live + saved), never mid-dodge, and puts the save's stance back when the
    bridge lets go: fists for Unarmed, the lightest right-hand weapon for OneHanded, the heaviest two-handed for TwoHanded.
  - `-ErStance` dev override.
- **Tested:**
  - Probe: 12027010 → 2027010 (one-handed) → 27010 (fists).
  - Both games: Skyrim Unarmed → ER fists, backstep anim 27010; the contact sheet shows the arms down after a roll.
  - `run-tests.ps1` green.
  - Not tested: drawing a weapon in Skyrim (the test save is unarmed).

## 2026-10-05: hands and feet follow the roll (finger/toe transforms carried)
- **User:** "the hands, wrists and feet stay in place when rolling, causing it to stretch".
- **Cause (probe):** fingers and toes aren't nodes in the player's tree. The skinned meshes skin them through loose animation-output
  transforms (hands: 34 of 36 bones outside the tree), which kept Skyrim's animation.
- **Fix:** `Pose.cpp` Carry moves each one rigidly with its nearest posed bone every frame. Pointers are re-read live.
- **Tested:** contact sheet of a forward roll: hands and feet stay on the limbs through the tumble; limb check 0°; hook ≤ 0.17 ms.
  A first probe that read nodes out of those pointers crashed Skyrim (they aren't nodes); removed.

## 2026-10-05: LOCO-PLAN stage A (partial): smoother pose, v5, whole-roll follow; movement smoothing stuck
- **Why:** the user's playtest said the roll is jittery, the distance is off, the arms stretch and running after a roll is weird.
  The user chose full ER locomotion (`docs/LOCO-PLAN.md`).
- **Changed:**
  - **Protocol v5:** `time_us` (QPC µs) in PlayerState/PoseState, PlayerState `cam_yaw`, PoseBone 20 → 24 (upper-arm twist), new
    `PoseBind` slot (ER bind directions → Skyrim fits).
  - `skse/src/bridge/Timeline.*` (new, adapted from SkyCraft): interpolates ER's stamped samples on Skyrim's clock.
  - Pose: limb-only fits, twist bones posed, pauldrons held at bind, fits from PoseBind.
  - Movement: interpolated steps, feed-forward steering, follows the whole roll unless the player steers (then hands back with the pose),
    `[move] smooth` stats.
  - `now_us`/`NowUs`.
- **Tested (agent-run):**
  - `run-tests.ps1` green.
  - Timeline delay 17-18 ms, **0 late frames** (was 90-222 per 5 s).
  - Limb check 0°; fits on limbs only (21/26/5-7°).
  - Skyrim follows the whole roll: ER 3.5 m, Skyrim 3.15-3.3 m (90-94%).
  - Contact sheets: rolls turn and tumble correctly.
- **Not solved:** the player's own motion still stalls on ~23 of 60 roll frames (ApplyCurrent refuses currents; 3 attempts,
  `docs/research/skyrim-hooks.md`). Stuck-rule handoff in STATUS. Stage B (ER locomotion) not started.

## 2026-10-04: POSE-PLAN step 5, ER roll animation plays in Skyrim
- **Changed:**
  - `skse/src/bridge/Pose.cpp` (rewritten from the F7 proof): the applier.
  - `skse/src/bridge/Rig.h` (new, written by a forked subagent): C++ mirror of `rig.rs` + Slerp/RotationArc, 11 selftest checks.
  - `Bridge.cpp` reads PoseState; `movement::RollHeading()`; `hooks/PlayerUpdate.cpp` dropped the proof calls.
  - `tools/game-input` gained `mouse dx dy`; `tools/screenshot.ps1` (new) takes contact sheets so the agent can look at the game.
- **Tested (agent-run, no user input):**
  - vs `fake-peer er --pose-always`: segment error 0° (limbs/spine), 9–11° (hands/feet).
  - Both games: forward roll plays (tuck, tumble, recover), Skyrim moved 99% of ER's distance.
  - Right roll: the body faces +90° and tumbles right.
  - Hook ≤ 0.14 ms, frame time unchanged. `run-tests.ps1` green.
- **Review (subagent) fixes:**
  - The pelvis offset now moves NPC COM, so hips and torso drop together (it had moved only the legs).
  - The facing is held through the blend-out, and the yaw is measured from the root's real heading.
  - `NiPointer` root (no dangling bones after a 3D rebuild). Resolve is retried every 60 frames.
  - Non-finite or non-unit pose data is rejected. No allocation per frame.
  - Retested: forward and left roll, whole body tumbles, 99% distance, ≤ 83 µs.
- **Not done:** fingers/toes, foot IK, interpolation above 60 fps, bind taken from the first skinned mesh (armour can tilt it), user's visual judgement.

## 2026-10-04: dev loop loads in without the user
- **Changed:**
  - `tools/game-input.psm1/.ps1` (new): window focus plus SendInput scan-code key scripts. A guard stops the script and releases
    every key if the game isn't in front.
  - `skse/src/bridge/AutoLoad.cpp` (new): with env `SKYRIMXER_AUTOLOAD` set, it loads that save when the main menu opens.
  - `launch.ps1 -AutoLoad` passes the newest `.ess`.
  - `dev.ps1` loads in by default (ER: E/Enter at the title until the player spawns; Skyrim: wait for the save, then focus).
    `-Manual` keeps the old flow; `-SkyrimKeys` plays a key script.
- **Tested:**
  - Skyrim alone: save loaded ~11 s after the menu, focused; the key script showed up as `move y=1` and `Dodge down/up`.
  - ER alone: in world after 8 presses, roll self-test as before.
  - Both: in world, CONNECTED, `PlayerState fresh`, Skyrim focused.
  - Guard: a script for an unfocused game sends nothing.
- **Findings:** `LoadMostRecentSaveGame` fails at main-menu open (the save list is still empty), so the scripts pass the save by name.
  Under Chrome Remote Desktop only `SwitchToThisWindow` takes the focus.

## 2026-10-04: POSE-PLAN step 4, ER pose writer (ER only)
- **Changed:** `protocol/src/rig.rs` (new): quaternion math, ER → Skyrim model basis as a matrix change (handles ER's left-handed axes),
  bind deltas, model-space compose; 7 unit tests. `er-plugin/src/pose_stream.rs` (new): resolves the skeleton once (20 bones by name,
  bind pose from the reference pose + parents, basis measured on it), then every frame in ChrIns_PostPhysics writes PoseState (deltas
  in Skyrim's basis, pelvis offset, yaw, Active while a dodge animation plays). `pose.rs` helpers shared, `park::is_dodge_anim` shared.
- **Tested:** two ER-only self-test runs (user pressed Continue). Basis right −X / forward −Z / up +Y, det −1. Backsteps: Active 81
  frames, pelvis 56°, drop 0.54 m. Rolls in 4 directions: all anim 27110 (direction is in yaw), Active 101 frames, pelvis 179°, drop
  0.84 m, write ≤ 0.27 ms. `cargo test` green. Skyrim doesn't apply the pose yet (step 5). Findings: `docs/research/elden-ring-pose.md`.

## 2026-10-04: POSE-PLAN step 3, protocol v4 PoseState (no game)
- **Changed:** protocol v4 (`Local\SkyrimXER_v4`): seqlock slot `PoseState` at 0x400 (368 B: flags `PoseFlag::Active`, frame, time_ms,
  bone_count, yaw, pelvis_offset[3], rot[4 × 20]), enum `PoseBone` (20 bones, parents first), constant `POSE_BONE_COUNT`. Regenerated.
  `fake-peer er` writes a pose every frame (Active while dodging, or always with `--pose-always`): pelvis yaw ±30°, right upper arm and
  left thigh pitch, 1 Hz. The C++ test peer logs `[pose] Active on/off` with the swing and a unit-length check.
- **Tested:** `tests/run-tests.ps1` all green: Rust + C++ PoseState round trips, interop A (Rust ER → C++ Skyrim: 20 bones, pelvis
  swing 30°, 0 bad quaternions per dodge). Both plugins rebuilt against v4; the Skyrim DLL deployed. Not run in either game (no behaviour change yet).

## 2026-10-04: POSE-PLAN step 2, Skyrim one-bone proof (Skyrim only)
- **Changed:**
  - `skse/src/bridge/Pose.{h,cpp}` (new): logs the third-person bone tree and each skinned geometry's bones (the bind-pose source) once. While F7 is held it turns the pelvis 45° right after `PlayerCharacter::Update`, using `local.rotate` + `UpdateDownwardPass`, and traces whether the write is still there at the next Update.
  - `bridge/Input`: `KeyHeld(scan code)`. `hooks/PlayerUpdate.cpp` calls the pose code before and after the original Update.
- **Tested:** Skyrim, keyboard. User: "F7 brought the legs out". A write after Update shows on screen; the animation re-poses the skeleton before the next Update.
  - A trial `UpdateAnimation` (vfunc 0x7D) hook showed it runs on a worker thread, once per frame. The hook was removed again.
  - `tests/run-tests.ps1` green.
- **Result:** ✔ step 2 accepted. Notes: `docs/research/skyrim-hooks.md` "Posing the player's skeleton". Also checked SkyCraft-SkateBridge and modern-warfare-2-ai (Skyrim + Skate plans). Neither has pose code yet; both plan the same `rig.rs` retarget we use. Next: step 3, protocol v4 PoseState.

## 2026-10-04: POSE-PLAN step 1, ER skeleton pose found (ER only)
- **Changed:**
  - `er-plugin/src/pose.rs` (new, research): `pose_probe=1` / `tools/dev.ps1 -ErPoseProbe`. A one-time object-graph walk from the opaque pose pointers, using safe reads (ReadProcessMemory on the own process) and RTTI names. It detects transform and bone-name arrays, then samples them during rolls, and logs the parents, the bind pose and the key bones.
  - Cargo: windows features `Win32_System_Diagnostics_Debug`, `Win32_System_Threading`.
- **Tested:** 2 ER runs with the roll self-test (user pressed Continue). Found `ChrIns+0x398` → `CSFD4LocationHkaPoseImporter` → hkaPose: skeleton (150 named bones, parents, bind) plus local and model pose. During a roll all model bones move (pelvis 0.94 → 0.27 m); idle stays under 1°. Notes: `docs/research/elden-ring-pose.md`.
- **Result:** ✔ step 1 accepted. Next: step 2, the Skyrim one-bone proof.

## 2026-10-04: Pose-streaming plan + reference study (docs only)
- **Changed:**
  - `docs/POSE-PLAN.md` (new): plan for P4 step 5. ER plays the roll on the hidden character; its bone pose is retargeted onto the Skyrim player every frame. The plan has 6 steps and starts with an ER pose memory probe. P4-PLAN step 5 links to it.
  - Studied Killcraft and 2010-rust-rewrite-mashup next to the existing references. THIRD-PARTY-NOTICES credits both.
  - The AI workflow doc (`docs/ai/CLAUDE.md` §14) now lists, for each problem (pose, interpolation, hits, HUD, camera, input, debug tools), where the reference projects solved it, plus a "when stuck" checklist.
- **Tested:** n/a (docs). Cited reference files checked to exist.
- **Result:** ✔ plan approved in direction by the user. Next: POSE-PLAN step 1 (ER pose probe).

## 2026-10-04: P4 step 5, attempt 3: vanilla Silent Roll (off; handed off)
- **Changed:** `skse/src/bridge/VanillaInput.{h,cpp}` (new): Sneak pressed through the game's SneakHandler with a synthetic ButtonEvent;
  Silent Roll perk added while the bridge is on (removed on F10 if added). `Movement.cpp`: trick = Sneak press → SprintStart once sneaking
  (retried) → forward move input for 0.48 s → sprint off + Sneak press; body turn via SetHeading + camera free rotation; chained rolls keep
  sneak; the "press + ER moving" chain rule only for backsteps (it falsely restarted dodges in a roll's recovery). `kSneakRollTrick` = false.
- **Tested:** both games, keyboard, 2 runs (~45 rolls). Sneak toggles cleanly, the roll plays, distances 98–101 %; but only forward (the
  third-person camera re-aligns the body each frame), and run 1's weapon-drawn rolls got no SprintStart. User: "it isn't working".
  `tests/run-tests.ps1` green.
- **Result:** ✖ vanilla route stopped (3 attempts). Next: stream ER's bone pose onto the Skyrim skeleton (STATUS handoff), user's direction
  ("do what the other merged games do").

## 2026-10-04: P4 step 4b: the hidden ER character is pinned (ER only)
- **Changed:** `er-plugin/src/park.rs` rewritten: every frame (ChrIns_PostPhysics) the character's horizontal step is added to a virtual
  position and the character is put back on its spot (physics position + `chr_proxy_pos_update_requested`); PlayerState.pos = the virtual
  position (`remote.rs`, same task as the pin in `lib.rs`). A jump > 1.5 m in one frame = new spot. `pin` config key / dev.ps1 `-ErNoPin`.
  `[park] dodge` line per dodge (virtual distance, max drift). dev.ps1 deletes the old ER log before launching (a just-killed ER's
  "in world" line ended `-WaitInWorld` runs early).
- **Tested:** ER self-test A/B: free 3.09/3.51/2.19/**0.07**/3.51/**0.08**/3.30/3.51 m (wall), pinned 3.51 m ×8, max drift 0.17 m,
  i-frames 27 frames. Both games (keyboard, W/A/S/D + 10 chained): 40+ rolls at 92–100 % of ER's distance, ER real position constant.
  `tests/run-tests.ps1` green.
- **Result:** ✔ user request "the ER character must not move unless rolling" (it doesn't move at all now). Skyrim unchanged.

## 2026-10-04: P4 step 4: the Skyrim player follows ER's rolls (both games)
- **Changed:** `skse/src/bridge/Movement.cpp` (new): while ER plays a dodge, ER's per-frame delta → character frame → Skyrim along the
  roll direction (camera yaw + move keys), applied closed-loop through `Actor::ApplyCurrent`; ends at ER's TAE movement-cancel window
  (new PlayerFlag MoveCancel, ER `game.rs`) or when ER stops; chained rolls/backsteps detected. `hooks/MoveSwallow.cpp`
  (MovementHandler::CanProcess, AE 208715): movement keys off during a dodge. Tap = dodge / hold = sprint: ER drops the stick after
  20 frames (`remote.rs` DASH_AFTER), Skyrim lets its sprint through as a fresh press (`SprintSwallow.cpp`). ER `park.rs`: the hidden
  character returns to its spot after each dodge. `bridge/AnimProbe.cpp`: bounded `[anim]` graph-event log. Roll-animation experiments
  (step 5) are in `Movement.cpp`, switched off.
- **Tested:** both games, keyboard, ~10 user runs. Final: rolls 96–99 % of ER's distance, a wall stops the roll, camera calm, rolls follow
  the camera, spam chains, backsteps chain, Shift hold sprints at ~510 u/s (vanilla sprint), the hidden character snaps back.
  `tests/run-tests.ps1` green (MoveCancel enum).
- **Result:** ✔ step 4. Step 5 (animation) stuck after 2 attempts, handoff in STATUS. Open: ER walls still shorten some rolls.

## 2026-10-04: P4 step 3: protocol v3, Skyrim's combat state drives ER's
- **Changed:** protocol v3 (`Local\SkyrimXER_v3`): InputState `_pad1` → `flags` (InputFlag InCombat, BridgeOn), PlayerFlag InCombat.
  Skyrim `Bridge.cpp` writes them (`IsInCombat()`, F10) and logs `[combat]` edges of both games. ER: `remote.rs` hands the wanted state to
  `combat::MirrorCombat` (ChrIns_AILogic, every frame, both ways); bridge off or stale input = ER decides. PlayerState reports ER's state.
- **Tested:** `tests/run-tests.ps1` green. Both games (keyboard): out of combat 3/3 rolls free; NPC set hostile (`startcombat player`)
  → 4/4 rolls cost 12 stamina; after `kill` 3/3 free again; all rolls with i-frames. Holding Sprint dashes (ER rule) and drains stamina
  in combat only.
- **Result:** ✔ step 3. Next: step 4, the Skyrim player follows the roll.

## 2026-10-04: P4 step 2: ER combat flag found and forced (ER self-test)
- **Changed:** `er-plugin/src/combat.rs`: `in_combat`/`set_in_combat` (CSChrDataModule +0x19a bit 0x40 = out of combat), `CombatWatch`
  (logs ER's combat-state edges every session), `ForceCombat` (`-ErForceCombat on|off`, one write per frame in ChrIns_AILogic), research
  tools behind `-ErDump`: `Dump` (raw PlayerIns/PlayerGameData/module snapshots → `logs/combat_dump.bin`) and `SpEffectWatch`.
  `log.rs` remembers the log dir; `config.rs` + `tools/dev.ps1` got the new switches.
- **Tested:** (1) SpEffect watch while the user rolled calm/in combat: no SpEffect explains it (ruled out). (2) Memory dump of the same
  sequence, analysed offline: one bit separates 7 free from 8 costly rolls. (3) Self-tests: written in every group, only ChrIns_NaviCache
  changes it back; forced in combat with no enemy, 5/5 rolls cost 12 stamina. The final single-group write (AILogic) follows from (3)
  and gets re-checked in step 3's both-games test.
- **Result:** ✔ step 2. Next: step 3, protocol v3 carries Skyrim's combat state to ER.

## 2026-10-04: P4 step 1: Sprint swallowed, F10 toggle, ER stick only around dodges
- **Changed:** P4 plan (`docs/P4-PLAN.md`, ROADMAP P4/P5 edited after the user's choices). New `skse/src/hooks/SprintSwallow.cpp`:
  `SprintHandler::CanProcess` (vtable AE 208717, vfunc 0x1) refuses Sprint presses while `bridge::SwallowSprint()` (bridge on + connected +
  ER in world); releases pass. `Input.cpp`: F10 toggles the bridge (off = nothing forwarded or swallowed), HUD message. ER `remote.rs`: the
  move stick is forwarded only while Dodge is held and 10 frames after, so the hidden character no longer walks with Skyrim.
- **Tested:** both games, keyboard (user). Bridged Shift+W ≈ 366 u/s (plain run 356), bridge off ≈ 499 (vanilla sprint); F10 both ways;
  plain walks sent nothing to ER; 5/5 directional rolls with i-frames.
- **Result:** ✔ step 1. Next: step 2, ER combat state (rolls free out of combat).

## 2026-10-04: Docs: "Made with Claude" + public CLAUDE.md
- **Changed:** README "Made with Claude" section (built with Claude Opus 5.5 in Claude Code, guided by a project CLAUDE.md) linking a new
  public copy `docs/ai/CLAUDE.md` (the project's agent instructions, cleaned of machine paths and private notes; copy it to the repo root to use it).
  Repository layout + Legal lines mention it.
- **Tested:** docs only; scanned the copy for private strings.

## 2026-10-04: P3: coordinate/yaw conversion measured (P3 done)
- **Changed:** bounded `[coords]` samples on both sides (Skyrim `Bridge.cpp` `SampleCoords`: position + `GetAngleZ()`; ER `remote.rs`
  `sample_coords` in ChrIns_PostPhysics: position + yaw + move), max 1500 lines per session, only while moving. New `protocol/src/coords.rs`
  (Local forward/right/up, ER/Skyrim delta ⇄ local, yaw delta) and its C++ mirror `skse/src/bridge/Coords.h`; unit tests in both languages
  with the measured segments. `bridge::OnFrame` now gets the player pointer from the hook.
- **Tested:** both games, the user walked W/D/S/A ~3 s each. Skyrim: W 2.7°, D 90.1°, S 176.5° off the heading (A hit an obstacle).
  ER: 63/63 fast samples run at yaw + 180°; W→D turned +90°, later facings predicted to 0.1° with "right = yaw + 90°". `tests/run-tests.ps1` green.
- **Result:** ✔ conventions in DESIGN §6 / `docs/research/coordinates.md`. **P3 done.** Next: P4 plan.

## 2026-10-04: P3: roll direction: Skyrim W/A/S/D → ER move stick (P3 accept met)
- **Changed:** `er-plugin/src/pad.rs`: analog slot helpers (`analog_slots`, `set_analog`, `poll_analog`, `set_move`, `move_polls`).
  `remote.rs`: fresh `move_x/move_y` from Skyrim → ER's MoveForwards/Backwards/Left/Right analog slots every frame while held, one release
  write, logged on change. `actions.rs`: `selftest=roll` (direction cycled per pulse, held 10 frames before the press until 5 after the release),
  the watch line shows the polled stick. Skyrim `bridge/Input.cpp`: Forward/Back/Strafe Left/Strafe Right user events → `MoveAxes()`;
  `Bridge.cpp` writes them into InputState and logs changes. `tools/dev.ps1 -ErSelfTest roll`. No protocol change (fields were in v2).
- **Tested:** ER self-test 1: forward/right rolled, back/left only backstepped (positive values ignored there). Self-test 2 with negative
  Backwards/Left: 7/7 short taps in all four directions roll (27110, dodge flag 27 frames). Both games (user, keyboard): 8/8 Sprint + direction
  → roll, Skyrim `IFrame on` 34–51 ms after the release and off ~450 ms later, HUD "i-frames yes"; 2/2 Sprint alone → backstep, "i-frames no".
  `tests/run-tests.ps1` green.
- **Result:** ✔ P3 accept met (stamina drop in combat earlier, i-frame window now). Next: coordinate/yaw test, the last P3 item.

## 2026-10-04: P3: i-frame window found (rolls set FLAG_AS_DODGING)
- **Changed:** `er-plugin/src/game.rs`: `iframe` = `action_modifiers_flags` bit 1 (`dodging`, TAE FLAG_AS_DODGING) or an invincibility bit
  (0/3/5), was bit 0 only. `actions.rs`: the Watcher logs HP, opens a watch window on every dodge animation (real presses too), and logs every HP
  loss with the modifier bits and the frame offset into the last dodge (`[probe] HIT`). Word-diff probe trimmed to the action-flag module.
- **Tested in-game (ER only):** (1) self-test backsteps out of combat with a special-effect diff and a per-task-group sample of the modifier bits:
  no effect and no bit during backsteps. (2) Combat run: a backstep-while-walking became a roll and showed bit 1 for ~27 frames.
  (3) User play test, ~20 rolls + some backsteps against an enemy: bit 1 set for 26–27 frames from each roll's first frame, **0 of 46 hits
  landed while it was set**; backsteps set nothing and were hit at +1..+34 frames. `tests/run-tests.ps1` green.
- **Result:** ✔ the i-frame source is found and published as `PlayerFlag::IFrame`. Skyrim's Sprint injects a backstep (no i-frames), so the
  both-games IFrame check moves to step 5 (roll direction). Details: `docs/research/elden-ring-state.md`.

## 2026-10-04: P3: i-frame search attempt 2 (not found) → handoff
- **Changed:** `er-plugin/src/actions.rs` Watcher: the dev-only word-diff probe now covers three regions (`action_flag`, `event` = CSChrEventModule,
  `chr_flags` = ChrIns+0x1c4..0x1cb), and the per-frame `[action]` line shows raw `ev_flags`.
- **Tested in-game (ER only, self-test, 10 backsteps):** no region shows an i-frame window. `ev_flags` is a constant 0xff; the chr_flags bits flip
  irregularly. Details: `docs/research/elden-ring-state.md`.
- **Result:** ✘ second failed attempt at the same problem → stuck-loop rule: handoff written (STATUS "Handoff notes"); the next try runs in a fresh session.

## 2026-10-04: Docs: README + roadmap refresh for the GitHub page
- **Changed:** README: author's "About this project" section, "What works today" / "Not working yet", input badge, Tested setup says
  keyboard + mouse (**controller support not added yet**, why, and that it's planned for P4), dev-loop commands updated. ROADMAP: P3
  ticks match reality, controller support added to P4, installer added to P8, new "Beyond the roadmap: ideas" section.
- **Tested:** docs only.

## 2026-10-04: P3: stamina drop shows in Skyrim (combat); torn-read fix; regen log squashed
- **Tested in-game (both games, ER visible, enemy aggroed in m60_42_37_00):** Left Shift in Skyrim → ER backstep 12027010 → Skyrim log + HUD
  `stamina 136→128` (a backstep costs 8 stamina in combat; out of combat it costs 0, which is why earlier runs showed none). The enemy's hits showed as
  HP 1450→1186. **I-frame flag still not found:** the action-flag word diff in combat shows only the anim-length markers (+0x10 bit 0,
  +0x40 bit 15 `disable_turning`, +0x1d8) and a combat bit (+0x10 bit 4).
- **Fixed:** a slot read can fail when the writer is pre-empted mid-write (all 64 tries torn). `run-tests` caught the watcher logging a false
  `stale` because of this. All readers (PlayerWatch C++/Rust, ER `DodgeFromSkyrim`, fake ER) now keep the last good copy and judge staleness by its age;
  before, ER would also have released a held Dodge for a frame. Stamina regen is now one `stamina regen A→B` line instead of one line per frame.
- **Tested:** `tests/run-tests.ps1` green 3× in a row; both plugins build.
- **Result:** the P3 accept's stamina part ✔ (with timestamps). The i-frame window is still open.

## 2026-10-04: P3: Skyrim HUD feedback for playtests
- **Changed:** `skse/src/bridge/Hud.cpp` (ShowHUDMessage, AE id 52933, checked with addrlib-check). `Bridge.cpp` `UpdateHud`: top-left
  notifications for ER connected/lost, ER character in/out of the world, and one summary per Dodge press 0.6 s later (ER anim reaction,
  stamina before→lowest, i-frames seen). Each one is also logged as `[hud]`.
- **Tested in-game (both games, keyboard):** connected + in-world messages appeared; 6 Left Shift taps → 6 × `ER dodge: anim 12027010 |
  stamina 136->136 | i-frames no` (backstep in animation group 12; the character's stance changed since step 2, idle = 12000000).
- **Result:** ✔. Still open: stamina/i-frames need combat (no enemy aggroed in these runs).

## 2026-10-04: P3 step 4b: the loop runs in both games (Skyrim Sprint → hidden ER backstep → state back)
- **Changed:** `er-plugin/src/remote.rs`: `DodgeFromSkyrim` (in WorldChrMan_Prepare, replaces the self-test unless `selftest=dodge`) holds
  Backstep (+ BackstepTapped on the press frame) while InputState is fresh + connected + Dodge held; it releases on stale/lost.
  `publish_state` writes PlayerState every frame (ChrIns_PostPhysics in world, FrameBegin while not in world). `bridge::shared()`.
  `actions.rs`: the watch window now always runs, plus a bounded word-diff probe of the action-flag module (i-frame search).
- **Tested in-game:** (1) real ER (hidden) + `fake-peer skyrim --dodge-every 4`: every press after spawn-in backstepped (anim 27010 +9 frames);
  fake-peer killed mid-hold → ER `InputState stale` 266 ms later + release, then the link timeout. (2) **Both games** (keyboard in
  Skyrim): Left Shift tap → `[ER] Dodge down` 9 ms later → `[SKY] anim 0→27010` ~170 ms after the press, for 3 taps + a hold;
  both games at 60 fps (Skyrim p50 17.0 ms). `tests/run-tests.ps1` passes.
- **Not shown yet:** stamina stays 101 and no i-frame flag gets set (real backsteps behave the same). Findings: `docs/research/elden-ring-state.md`.
- **Result:** step 4 loop ✔. The P3 accept's stamina-drop + i-frame part is still open.

## 2026-10-04: P3 step 4a: Skyrim frame hook + Sprint → InputState (vs fake ER)
- **Changed:** `skse/src/hooks/PlayerUpdate.cpp` (PlayerCharacter::Update vfunc 0xAD), `skse/src/bridge/Input.cpp` (input sink: Sprint
  user event held → Dodge; logs the first event per device and each user event once), `Bridge.cpp` `OnFrame` (writes InputState, reads
  PlayerState through `PlayerWatch.h`, `[perf]` p50/p95/p99 every 5 s, frames now in Heartbeat). `tools/addrlib-check.ps1` checks
  Address Library ids. `launch.ps1 -SkyrimVia steam|loader` (default loader).
- **Tested in-game (Skyrim + `fake-peer er`, keyboard):** the ids were checked first (all present). Sprint (Left Shift) taps → `Dodge down` →
  PlayerState stamina 100→80 + IFrame on ~17 ms later, IFrame off after 1 s, regen; a hold works as well. Frame time p50 16.8 ms, hook ≈ 2 µs.
  Clean exit → fake ER saw Bye. `tests/run-tests.ps1` passes.
- **Found:** Skyrim gets **no gamepad events** when started by skse64_loader outside Steam (no Steam Input). The Steam launch option
  `"...skse64_loader.exe" %command%` fails ("too many free args"). The controller is deferred (user decision). Notes: `docs/research/skyrim-hooks.md`.
- **Result:** step 4 Skyrim side ✔ (keyboard). Next: ER side (injector from InputState, PlayerState publisher).

## 2026-10-04: P3 step 3 done: protocol v2 seqlock slots
- **Changed:** schema v2 (`Local\SkyrimXER_v2`): `InputState` slot @0x200 (sky→er), `PlayerState` slot @0x300 (er→sky), `Button`/`PlayerFlag`
  bit enums, `SLOT_STALE_MS`/`SLOT_READ_TRIES`. protogen requires `SLOT_*` structs to start with `seq: u32`. New `protocol/src/slot.rs` + C++ mirror
  `skse/src/bridge/Slot.h` (seqlock, body copied as u32 atomics). `LinkShared` (region + `connected`) on both links for lock-free game-thread access,
  published only after a valid join. fake-peer + C++ peer: link thread + frame loop, Dodge pulses (`--dodge-every`), fake ER dodge model, PlayerState edge log.
- **Tested (no game):** `tests/run-tests.ps1` all green. Covered: protogen (incl. the slot rule); Rust slot tests (round trip, dead-writer repair,
  fresh, 0.5 s concurrent torn-read check); link `LinkShared` asserts; the C++ selftest (same + a threaded torn-read check, ~9 M reads); interop A
  (C++ Skyrim Dodge → Rust ER dodges → C++ sees stamina 100→80 and IFrame on/off ~31 ms later, stale once after the crash); interop B. Both
  plugins rebuilt + deployed (`dev.ps1 -NoLaunch`).
- **Result:** P3 step 3 ✔. Next: step 4 (wire the loop in both games).

## 2026-10-04: P3 step 2 done: injected backstep works with ER hidden
- **Changed:** `er-plugin/src/window.rs`: the focus spoof also runs at WorldChrMan_Prepare (PadStep re-sets `is_back_ground_window`
  every frame while ER isn't foreground). `er-plugin/src/focus.rs`: change-only focus-flag probe (dev, `probe=1`).
- **Tested in-game (ER only):** focus probe run (visible, user switched windows): backsteps stopped while another window was foreground,
  `is_back_ground_window` was 1 after PadStep. After the fix, hidden run, hands off: 7/8 pulses backstepped (the miss was during spawn-in), 60 fps.
  `tests/run-tests.ps1` passes.
- **Result:** P3 step 2 ✔. Open note: i-frame/dodging flags read 0 during backsteps (real ones too), see research note.

## 2026-10-04: P3 step 2: injected backstep works (window focused)
- **Changed:** `er-plugin/src/pad.rs` (`describe`, `poll_mask`, `layers`: read-only input diagnostics), `actions.rs` (wide per-frame probe:
  full action masks, all key polls, device layers; self-test also raises `BackstepTapped` on the press frame; holds 4/9/30). `tools/dev.ps1`
  refuses to launch ER without Steam running.
- **Tested in-game (ER only, window visible, 2 runs):** probe compared a real DualSense tap with ours: the real one also raises
  `BackstepTapped` for one frame, which makes the release a backstep request (bit 16). With that added, the self-test backsteps
  (anim 27010) on its own; the user saw it. `tests/run-tests.ps1` passes.
- **Result:** step 2 ✔ while ER is focused. Still open: the action module ignores the pad while ER isn't the foreground window
  (the hidden case). Details: `docs/research/elden-ring-input.md` ("session 3").

## 2026-10-04: P3 step 2 diagnostics (dodge injection, still blocked)
- **Changed:** `er-plugin/src/actions.rs`: the probe logs the action-request gating masks every 120 frames (possible, cancels, disabled, queued,
  animation flags) and a queued bit per task group. The self-test cycles hold lengths 4/8/12/20/30 frames and logs the engine's hold timer. Dev-only, off by default.
- **Tested in-game (ER only, 3 runs, window visible):** the virtual press reaches the action-request module like a real press; no dodge for any hold
  length; a real DualSense tap in the same session backsteps with identical module values.
- **Result:** step 2 still ✘, narrowed: the module isn't the missing link. Findings in `docs/research/elden-ring-input.md` ("session 2 results").
  Also: me3 needs Steam running (`Steam is required to run this game` in the me3 log).

## 2026-10-04: Phase 3 started: ER runs hidden; dodge injection research
- **Changed:** P3 plan (`docs/P3-PLAN.md`). `er-plugin`: `window.rs` (each frame: focus flags spoofed; hide the window while the player is in
  the world, show it again at the title), `game.rs` (player snapshot: HP/FP/stamina, anim, i-frame/dodging/hyperarmor flags, poise, position,
  yaw, map), `config.rs` (`skyrimxer_er.cfg` next to the DLL), `actions.rs` + `pad.rs` (dodge self-test + frame-phase probe, dev-only, off by default).
  `tools/dev.ps1`: `-Restart`, `-ErVisible`, `-ErSelfTest`, `-ErInjectGroup`, `-ErProbe`. `tools/build.ps1`: retries the DLL copy after a kill.
- **Tested in-game (ER only):** the window hides on load-in and ER keeps running at 60 fps hidden; the state line reads stamina 101/101, HP 455/455,
  map m10_01_00_00. Probe of a real dodge press recorded where ER sets its action bits (`docs/research/elden-ring-input.md`).
- **Result:** step 1 ✔. Step 2 (injected dodge) ✘ so far: writing `action_requests` is overwritten by the engine; a held virtual pad key is seen
  by ER's pad poll but doesn't reach the player's action requests. Next hypotheses are in the research note.
- **Workflow pass (same day):** README rewritten (diagram, progress table, tested setup incl. **DualSense over USB** as the only test controller,
  safety section, one-command dev table). `dev.ps1 -WaitInWorld N` waits for the load-in and prints the ER results itself;
  `tools/stop-games.ps1` (+ shared `Stop-Game` in `common.psm1`); `tools/README.md` refreshed. DESIGN §9: controller open question.
  Tested: scripts parse, `dev.ps1 -Target er -NoLaunch` OK, `stop-games.ps1` closed a hidden ER.

## 2026-10-04: Phase 2 complete (shared memory + heartbeat)
- **Changed:** Protocol schema v1 (`protocol/schema/messages.toml`) + generator `tools/protogen` (rejects implicit padding, checks the region
  map, emits size/offset asserts for C++ and Rust, `--check` stale test). Rust crate `skyrimxer-protocol` (region, SPSC rings, link state
  machine) used by `er-plugin` and `tools/fake-peer`. C++ mirror `skse/src/bridge/Link.cpp` + `Bridge.cpp`, and `skyrimxer_link_test.exe`.
  `tests/run-tests.ps1`. `collect-logs.ps1` now prints `[link]` lines (heartbeat lines summarised).
- **Tested without games:** `tests/run-tests.ps1` passes: 10 Rust tests (ring wrap/full/corrupt, handshake, timeout, reconnect, Bye, restart,
  header mismatch), 9 C++ selftest checks, Rust↔C++ cross-process (crash → timeout; clean exit → Bye).
- **Tested in-game (agent-run, both games at the main menu):**
  - ER created the region, Skyrim opened it on kDataLoaded: `CONNECTED: handshake ok` on both sides within 50 ms.
  - ~7 min connected: heartbeat events every 5 s both ways, 0 dropped. ER `frames` rose ~300 per 5 s (≈ 60 fps while unfocused at the title).
  - `eldenring.exe` force-killed → Skyrim: `LOST: ER ... heartbeat timeout (last beat 2031 ms ago)`, Skyrim kept running.
  - ER relaunched alone → `attach#2`, reconnected without restarting Skyrim.
  - Skyrim window closed → clean exit, ER: `LOST: Skyrim ... shutting down` + `Skyrim said Bye (reason=Quit)`, ER kept running. No crash logs.
- **Bugs fixed during the work:** Hello read before its sender was detected looked like a restart (fix: check peer before reading events, and
  answer a Hello received while connected). ER log level name `warn` → `warning` to match spdlog.
- **Next:** P3.

## 2026-10-04: Workflow + repo hygiene
- **Changed:** Added `tools/dev.ps1` (build → deploy → backup → launch → wait for plugin ready lines → collect logs). AI-agent files are now
  local-only, and history was rewritten so they never appear in it. Logging format moved to `docs/DESIGN.md` §8 and the release checklist to
  `release/README.md`, so public docs are self-contained. Added a local pre-commit guard against private data and game/binary files.
- **Tested:** `dev.ps1 -NoLaunch` (full build + deploy in 9 s). The pre-commit guard blocks a staged Windows user path and passes clean files.
  The full `dev.ps1` launch path gets its first real run in P2.

## 2026-10-04: Phase 1 complete (hello world from both plugins)
- **Changed:** Added `skse/` (CMake + Ninja preset, VS-bundled vcpkg pinned to registry baseline 00c5775, CommonLibVR-ng submodule @39f9d07,
  AE only, `src/main.cpp`). Added the Cargo workspace + `er-plugin/` (eldenring-rs @59fbd3b, `lib.rs` + `log.rs`, panic=unwind).
  Added tools: build, deploy, backup-saves, launch, collect-logs, common.psm1. Added `local/paths.json`.
- **Tested in-game (both games at once via `tools/launch.ps1`):**
  - Skyrim: `SkyrimXER v0.1.0.0 loaded, runtime 1.7.104.0` → `kDataLoaded`. skse64.log: "loaded correctly".
  - ER: `loaded` → `game version supported, task system ready` → `per-frame task registered` → user loaded the test character →
    `main player spawned (in world)` → `main player gone (menu/loading)`. me3 attach config: our native, `savefile: skyrimxer.sl2`, `start_online: false`.
- **Bugs fixed:** (1) `SKSE::Init` replaced our logger, fixed with `SKSE::InitInfo{ .log = false }`, re-tested OK. (2) build.ps1 aborted on the
  VS dev shell's harmless stderr under PS 5.1 + `Stop`, fixed by running that call with `Continue` and checking the exit code.
- **Build times:** SKSE cold 385 s / incremental 10 s. ER cold 4m39s / incremental ~2 s.
- **Next:** P2 shared memory + heartbeat.

## 2026-10-03: Phase 0 complete
- **Decided:** CommonLibVR-ng 10.1.0 (`ae` preset; has `RUNTIME_SSE_1_7_104`). eldenring-rs v0.14 (supports exe 2.7.1.0 WW). libER dropped.
  Project license **GPL-3.0-or-later** (user choice; every CommonLib supporting 1.7.104 is GPL-3.0). `LICENSE` added.
- **Found:** ER input can be injected as `ChrActions` bits on `CSChrActionRequestModule` (no OS keys). Focus flag is
  `DLUserInputManagerImpl.is_game_window_focused`. Notes in `docs/research/elden-ring-input.md`.
- **Tested:** eldenring-rs `apply-speffect` example built (29 s). CommonLib `ae` built with VS 2026 + bundled vcpkg (590 s), unit tests 25/25.
  `tools/setup-check.ps1` written and passing.
- **Moved to P3:** arena choice, movement-direction source, both-games-at-once performance.

## 2026-10-03: me3 smoke test (Elden Ring, no mods)
- **Ran:** `me3 launch -g eldenring --savefile skyrimxer.sl2` after backing up ER saves to `local/save-backups/`.
- **Result ✔:** me3 attached to "ELDEN RING 1.17.1.0 Worldwide". Hooks applied (filesystem, allocators, assets, skip_logos). Arxan detected
  and the attach deferred cleanly. No EAC process. All save I/O redirected to `skyrimxer.sl2` (a copy of ER0000 made on first use), and
  `ER0000.sl2` was untouched. Zero WARN/ERROR lines. Log: `logs/me3-smoke-stderr.txt` + `%LOCALAPPDATA%\garyttierney\me3\data\logs\transient-profile\`.
- **Next:** Phase 0 library checks (CommonLib fork for 1.7.104, eldenring-rs for 1.17.1).

## 2026-10-03: Tooling installed
- **Changed:** Installed me3 0.13.0 (official signed installer, silent). Installed Crash Logger 1.25.0 (GitHub) into `Data\SKSE\Plugins`.
  Found that the installed SKSE 2.2.6 targeted 1.6.1170, which doesn't match the game's 1.7.104. Replaced it with **SKSE 2.3.1**
  (127 overwritten files backed up to `local/backups/skse-2.2.6/`). Installed **Address Library v13 All in One** (contains `versionlib-1-7-104-0.bin`).
  Every change is listed in `local/install-manifest.json`.
- **Found:** me3 profiles can't store a save file, so launches must pass `--savefile skyrimxer.sl2`. me3 blocks matchmaking by default.
  MO2 is installed but has no instance, so files went straight into `Data\`.
- **Tested:** File versions checked (`skse64_1_7_104.dll` / loader = 2.3.1). `me3 info` → installation Found. **Not yet tested in-game.**
- **In-game check (same day):** Launched through `skse64_loader.exe`. `skse64.log`: SKSE 2.3.1 initialized, CrashLogger "loaded correctly",
  kDataLoaded reached. ✔ The Skyrim side of the toolchain works.
- **Next:** me3 smoke test with Elden Ring (no natives, `--savefile skyrimxer.sl2`), then the rest of Phase 0.

## 2026-10-03: Project scaffold
- **Changed:** Created the repo structure, local agent-rules file, design/roadmap/recon docs, whitelist `.gitignore`, me3 profile template,
  and folder READMEs. Ran `git init`. Cloned the reference repos into `reference/` (ignored).
- **Environment found:** Skyrim `SkyrimSE.exe` 1.7.104.0 + SKSE 2.2.6 (no Address Library yet). Elden Ring `eldenring.exe` 2.7.1.0.
  VS 2026 C++ ✔, Rust ✔, git ✔. Missing: me3, Address Library.
- **Decision:** Skyrim plugin uses SkyCraft's CMake + `alandtse/CommonLibVR` (ng) submodule setup, so xmake isn't needed. Fake-peer test processes are planned for P2.
- **Tested:** `git status --ignored` confirms only docs/config are tracked and game/build/local folders are ignored.
- **Next:** Phase 0 (checking tools & versions). See `STATUS.md`.
