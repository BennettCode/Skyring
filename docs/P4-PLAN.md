# P4 plan: movement & defense

## Context
P3 is done (497bc92): Skyrim Sprint (+WASD) → hidden ER rolls/backsteps by ER's rules; ER stamina/anim/i-frame flag/pos/yaw come back
every frame; coordinate conversion measured (`protocol/src/coords.rs` ⇄ `skse/src/bridge/Coords.h`). P4 makes the roll *real in Skyrim*.
**ROADMAP P4 accept:** the user rolls through an NPC's attack without taking damage, and spamming rolls is limited by stamina.

**User decisions (2026-10-04):**
- Roll animation: **vanilla only** (Skyrim's sneak "Silent Roll" for rolls; other directions may slide for now). No extra mods.
- Stamina: rolls cost stamina **when the Skyrim player is in combat** (mirrors ER's own rule), so ER must be put in its combat state.
- Swallow **Sprint only** in P4 (F10 toggles the bridge). Attack/block swallowing moves to P5 with their forwarding (ROADMAP edit).
- DualSense in Skyrim = **last P4 step**; everything before is keyboard/mouse.

**Facts this builds on:** ER rolls are free out of combat (backstep cost 8 with an enemy aggroed); roll i-frames = `action_modifiers_flags`
bit 1 for 26–27 frames (`docs/research/elden-ring-state.md`). PlayerState already carries `pos[3]` + `yaw` → Skyrim can compute ER's
per-frame displacement itself (no new field for movement). Skyrim Data is vanilla (no behavior mods). SkyCraft has the needed hook
patterns: `PlayerControls` vtable vfunc 0x1 filter (`reference/SkyCraft/skse/src/Input.cpp:238`), `HandleHealthDamage` vfunc 0x104 and the
hit pipeline ids 38627/38586/43995 with a call-site byte check (`Combat.cpp:86, 227`).

## Steps (each: build → test → user-confirmed when it needs the game → commit + push)
First: copy this plan to `docs/P4-PLAN.md`, ROADMAP P4/P5 edits (swallow list, vanilla anim, controller last).

### 1. Swallow Sprint + F10 toggle + park the ER stick (Skyrim + ER, no protocol change)
- Skyrim `skse/src/hooks/SprintSwallow.cpp` (own file + comment): `RE::VTABLE_SprintHandler[0]` vfunc 0x1 (`CanProcess`) → false while the
  bridge is on and linked; fallback = SkyCraft's `PlayerControls` 0x1 event-list filter. **`tools/addrlib-check.ps1` on the VTABLE id first.**
- `Input.cpp`: F10 (DIK 0x44) flips `bridgeOn` (HUD + log). Bridge off → InputState buttons/move = 0 and vanilla sprint returns.
- ER `remote.rs`: forward the move stick only from Dodge down until ~10 frames after release (not on every WASD frame), so the hidden
  character stays parked instead of walking/circling. Log the window.
- **Test:** `fake-peer er` + Skyrim (Shift: no vanilla sprint, Dodge in log; F10: vanilla sprint back), then both games: Sprint+WASD rolls still 8/8.
- **Result (2026-10-04, both games, keyboard):** Shift+W while bridged ran at ~366 u/s (plain W 356); bridge off reached ~499 (vanilla
  sprint). F10 toggled both ways (HUD + log). Plain W walks sent no stick to ER (parked); 5/5 directional rolls with i-frames; 2 long holds
  dashed in ER (hold = dash). Seen: after a roll the ER character sometimes drifts with no input (anim 26032000): check in step 2/4.

### 2. ER combat state (ER only, research; stop and hand off after 2 failed attempts)
- Find what makes ER charge stamina in combat: probe diff (like i-frame attempt 3) between aggroed and calm near the parked enemy:
  PlayerIns/ChrIns flags, SpEffects, `WorldChrMan`/targeting state (`targeting.rs` `is_battle_state` is AI-side). Note in
  `docs/research/elden-ring-state.md`.
- Then a self-test switch (`-ErSelfTest combat`) that forces the found state with no enemy and checks a roll costs stamina.
- Afterwards the user moves the ER test character once to **flat open ground with no enemies** (ER-side walls shorten rolls; enemies hurt it).
- **Result (2026-10-04):** attempt 1 (SpEffect watch: an out-of-combat effect with `consume_stamina_rate` 0) ruled out. Attempt 2 (raw
  memory dump, analysed offline) found it: `CSChrDataModule` +0x19a bit 0x40 = out of combat. ER recomputes it in ChrIns_NaviCache every
  frame; written in ChrIns_AILogic it holds, and forced "in combat" with no enemy 5/5 rolls cost 12 stamina. `combat::set_in_combat`,
  `-ErForceCombat on|off`, `-ErDump`. Details: `docs/research/elden-ring-state.md`.

### 3. Protocol v3 (no game; `tests/run-tests.ps1` green)
- `messages.toml`: version 3 / `Local\SkyrimXER_v3`; InputState `_pad1` → `flags` (new enum `InputFlag`: `InCombat`, `BridgeOn`);
  `PlayerFlag` + `InCombat` (ER's own state, for logs). Layout size unchanged. Regenerate, update fake peers + tests, DESIGN §4.
- Skyrim writes `InCombat` = `player->IsInCombat()`; ER forces/releases the step-2 state from it (fail-safe: stale → release).
- **Result (2026-10-04, both games, keyboard, NPC made hostile with `startcombat player`, ended with `kill`):** calm 3/3 rolls free,
  Skyrim combat 4/4 rolls cost 12 (136→124), after the fight 3/3 free; all with i-frames. Console pauses make the input stale and ER falls
  back to its own state (as designed). `combat::MirrorCombat` (ChrIns_AILogic), `[combat]` edge lines on both sides. run-tests green.

### 4. Skyrim player follows the ER roll (both; the core)
- New `skse/src/bridge/Movement.cpp`: while ER's anim is a dodge (`anim % 1e6` in the 27xxx dodge set, logged/measured), take ER's horizontal
  pos delta since the last *ER frame* seen (absolute positions → nothing lost at mismatched frame rates), `er_delta_to_local` with ER yaw,
  then local → Skyrim world with the Skyrim heading. At roll start turn the Skyrim player to the input direction (camera yaw + stick
  angle), like an unlocked ER roll; backsteps keep heading. Skyrim's own WASD movement is suppressed while the dodge runs.
- **Apply method (prove with a test first):** (a) character-controller velocity = delta/dt so Skyrim collision stays in charge (hook point
  found from CommonLib/open-source mods, id checked with addrlib-check, noted in `docs/research/skyrim-hooks.md`); fallback (b) `SetPosition`
  clamped by a collision cast. Log per roll: ER distance vs Skyrim distance, frames, blocked-by-wall.
- **Test (both games, keyboard):** 8 directional rolls + 2 backsteps in the open + 1 roll into a wall. Accept: distance within ±10 % of
  ER's, no clipping, heading correct.
- **Result (2026-10-04, many both-games runs):** `skse/src/bridge/Movement.cpp`. Apply method (a) works: `Actor::ApplyCurrent` (vfunc 0x9D)
  with a closed loop (a target point moves with ER's per-frame delta; the player is steered onto it, lead capped at 60 units so walls stop
  it) → rolls land at **96–99 %** of ER's distance; a roll into a wall stops at the wall. Open-loop versions were 79–150 %.
  What the playtests changed: no body rotation (turning the player flips the third-person camera); the roll direction = **camera yaw** +
  move-key angle (the body lags a turning camera); Skyrim's movement keys are swallowed during a dodge (`hooks/MoveSwallow.cpp`,
  MovementHandler::CanProcess, AE 208715) and given back at ER's **TAE movement-cancel window** (new PlayerFlag MoveCancel, ~43 frames)
  or when ER stops moving; chained rolls = new i-frame window after a press, chained backsteps = press + ER moving again.
  **Tap = dodge, hold = sprint:** past 20 frames ER drops the stick and Skyrim's own sprint is let through (presented as a fresh press).
  **Parking (`er-plugin/src/park.rs`):** the hidden character returns to its spot after every dodge (physics position +
  `chr_proxy_pos_update_requested`); the spot only follows the character while Skyrim isn't driving it.
  ~~Open: ER-side walls still shorten rolls.~~ **Step 4b (pin, 2026-10-04):** `park.rs` now puts the character back on its spot
  every frame and integrates the per-frame step into a virtual position that PlayerState.pos carries (Skyrim unchanged). A/B self-test:
  free rolls near the wall 0.07–3.51 m, pinned 3.51 m every time; both games: 40+ rolls at 92–100 % of ER's distance, ER character
  never moved. `pin=0` (dev.ps1 `-ErNoPin`) for debugging.

### 5. Roll animation, vanilla (Skyrim; experiment, 2 tries)
- Find the Silent Roll graph event: `BSTEventSink<BSAnimationGraphEvent>` on the player logs events while the user does a vanilla sneak
  roll (bridge off, perk added once via console on the test save). Then send it with `NotifyAnimationGraph` at our roll start and cancel its
  root motion so step 4's movement isn't doubled. Fallback: slide (documented in README "Not working yet").
- **Status (2026-10-04): 2 attempts, not working, handed off.** (1) `NotifyAnimationGraph("SneakSprintStartRoll")` returns true but
  nothing plays. (2) Sneak trick (SneakStart, SprintStart next frame, undone at the end) starts the roll (graph notifies `tailSprint`,
  `StartAnimatedCameraDelta`) but `SprintStop` follows after ~8 frames (vanilla ~29) because the dodge zeroes the move input; about half
  the tries only crouch, and the HUD sneak eye stays up. Switched off (`kSneakRollTrick`). Vanilla sneak roll (Silent Roll perk, user's
  test save) notifies only `tailSneakLocomotion` → `tailSprint` → `SprintStop` ~0.48 s later.
- **Attempt 3 (2026-10-04, user chose vanilla Silent Roll):** Sneak pressed through the game's own `SneakHandler` (a "Sneak"
  `ButtonEvent`, `bridge/VanillaInput.cpp`) + forward move input + perk added while the bridge is on. Works: sneak goes in and out
  cleanly every time (no lingering eye), the roll plays (`tailSprint` + `StartAnimatedCameraDelta`), distances 98–101 %. Fails: the roll
  only plays **forward** (where the camera looks) because the third-person camera puts the body back on the camera yaw every frame;
  `SetHeading` + `ThirdPersonState.freeRotation` had no effect. With a weapon drawn the graph refused `SprintStart` (run 1). Off again.
- **Next (user, 2026-10-04: "do what the other merged games do"):** GTA San AnSkateas streams the hidden engine's **bone pose** onto the
  host character every frame (no animation files converted or shipped). Here: ER plays the roll on the hidden character, the ER plugin
  reads its skeleton pose, Skyrim writes it onto the player's skeleton. New plan + research (ER pose location, bone map, protocol slot)
  in a fresh chat; it would animate every ER action (P5 attacks too), so it may become its own phase.
  **Plan: `docs/POSE-PLAN.md`** (user chose pose streaming, 2026-10-04).

### 6. Stamina bar mirrors ER (Skyrim): done 2026-10-05
- **Result:** `skse/src/bridge/Stamina.cpp` (FalloutCraft fraction mirror). Forced combat (`-ErForceCombat on`): rolls + sprint drained
  ER 136 → 8 and Skyrim 400 → 24 (same share every frame, the damage modifier always took), refill tracked to full, F10 off/on switches the mirror.
- Every frame with fresh PlayerState: Skyrim Stamina current = Skyrim max × ER stamina/max (damage-modifier delta via
  `ActorValueOwner::RestoreActorValue/DamageActorValue`), logged on change. Stale/bridge off → stop touching it.
- "Out of stamina = no roll" is ER's own gating. **Test:** in Skyrim combat, spam Sprint+W: bar drains, ER refuses rolls at 0
  (log: Dodge press with no dodge anim), bar refills.

### 7. NPC hits during ER i-frames are cancelled (Skyrim): melee done 2026-10-05
- **Result:** option (a). `skse/src/hooks/PlayerHit.cpp` replaces the `call 38586` at 38627+0x4A8 (SkyCraft byte check, trampoline 64
  in `main.cpp`); `bridge::OnFrame` sets the i-frame flag from fresh PlayerState (`kIFrame`), false when stale / link off. A hit on the
  player while it's set is dropped whole (no damage, stagger, hit event). Wolves (`player.placeatme 23abe N`): 6 hits skipped, all inside
  logged IFrame windows; 12 landed outside them; F10 off → 7/7 landed. **Not done:** projectile and magic hits (other paths) → P5.
- `skse/src/hooks/PlayerHit.cpp`: skip the hit on the player while `IFrame` is fresh. Try (a) hook the hit-application call (id 38586 at the
  melee site 38627+0x4A8 with SkyCraft's byte check; find the projectile/magic sites) and log every skipped/landed hit with i-frame state;
  (b) fallback: player ghost flag during i-frames. All ids via addrlib-check first.
- **Test = P4 accept:** a hostile NPC (spawned via console on the test save) attacks; the user rolls through swings. Log: hits during
  i-frames skipped, others land; stamina spam limited.

### 8. DualSense in Skyrim: done 2026-10-05 (user pad test passed)
- **Result:** the plugin reads the DualSense over HID (`bridge/Gamepad.cpp`, parser + layouts `bridge/PadReport.h`) and answers Skyrim's
  `XInputGetState` import (`hooks/XInput.cpp`; Skyrim imports XInput by ordinal, so the IAT is walked by hand). User layout = Elden
  Ring's buttons (README). R1/R2/L1/L2 → protocol v8 `Button` Attack/StrongAttack/Guard/Skill → ER virtual keys (`remote.rs`).
  ER read the physical pad itself (libScePad; R1 played ER attacks): while bridged, `pad::clear_virtual` releases every action key and
  analog value after PadStep, then ours are written. Mouse attacks → ER, vanilla attack/block off (`hooks/AttackSwallow.cpp`).
- **Found on the way:** Steam Input exposes a second copy of the DualSense as an XInput pad (no touchpad, slow triggers). It made the
  touchpad open the Journal and the Journal tabs ignore the triggers. Our HID pad now always wins; other pads are hidden while it's open.
- **Agent pad tests:** `tools/pad-input.ps1` / `dev.ps1 -SkyrimPad` (dev virtual pad, `bridge/PadScript.*`): map, Journal tabs, Tween
  verified by screenshot before the user's test.
- Original options (kept for reference):
- Options: `cmd /c start` launch option, non-Steam shortcut, or reading the pad in the plugin. Also stop the hidden ER reading the same pad.

## Files (main)
`protocol/schema/messages.toml` + `generated/*`, `tools/fake-peer`, `tests/run-tests.ps1`, `skse/src/bridge/{Bridge,Input,Movement,Coords.h}`,
`skse/src/hooks/{SprintSwallow,PlayerHit}.cpp`, `skse/CMakeLists.txt`, `er-plugin/src/{remote,game,actions}.rs` (+ `combat.rs`),
`tools/dev.ps1` (`-ErSelfTest combat`), docs (P4-PLAN, ROADMAP, DESIGN §3/§4, research, STATUS, MODLOG, README), THIRD-PARTY-NOTICES.

## Verification
- `tests/run-tests.ps1` green before any protocol/link commit; `cargo run -p protogen -- --check` clean.
- Every game run via `tools/dev.ps1` (backup, me3 offline); user steps in keyboard/mouse; the agent reads `[input]/[state]/[move]/[hit]` lines.
- Fail-safe: kill ER mid-roll → Skyrim stops applying movement/stamina/i-frames within 250 ms (stale), logs it once; F10 off = vanilla game.
