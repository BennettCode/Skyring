# ROADMAP

Rule: don't start a phase until the previous phase's acceptance check is **confirmed by the user in-game** and committed.
Tick boxes as you go. Keep each phase small. If a phase grows, split it.

## P0: Checking tools & versions
- [x] `docs/RECON.md` sections A, B, D complete (C = both-games-at-once numbers moved to P3, which runs both for real)
- [x] Missing tools installed (SKSE 2.3.1, Address Library v13, Crash Logger, me3 0.13.0)
- [x] `tools/setup-check.ps1` passes (2026-10-03)
- [x] CommonLib `ae` build succeeds with VS 2026 + bundled vcpkg (590 s cold, `[unit]` tests 25/25 pass, 2026-10-03)
- [x] User creates a dedicated ER test character in `skyrimxer.sl2` (2026-10-03)
**Accept:** the user approves the decisions in RECON §D.

## P1: Hello world (both sides)
- [x] `skse/`: plugin builds. On load it writes `SkyrimXER v0.1.0.0 loaded, runtime 1.7.104.0`, then `kDataLoaded`
- [x] `er-plugin/`: cdylib builds and gets loaded by me3. Logs `loaded`, `game version supported, task system ready`, `per-frame task registered`,
  and `main player spawned/gone` (proves eldenring-rs reads `WorldChrMan` on this exe)
- [x] `tools/build.ps1`, `deploy.ps1` (+`-Undo`, `-WhatIf`), `backup-saves.ps1`, `launch.ps1`, `collect-logs.ps1`
**Accept:** ✔ **DONE 2026-10-04.** Both log lines present after one `tools/launch.ps1` run with both games at once. Logs in `logs/20261004-000407/`.

## P2: Shared memory + heartbeat
- [x] `protocol/schema` v1 header + Hello/Bye/Heartbeat. Generator writes `.h` + `.rs`. Layout tests pass on both sides
- [x] Stand-in peers (`tools/fake-peer`, Rust; `skyrimxer_link_test.exe peer`, C++) so each side can be tested without the other game
- [x] Both sides open the region, exchange Hello, and log the other side's heartbeat every 5 s (2026-10-04)
- [x] Kill ER → Skyrim logs a timeout and stays stable. Quit Skyrim → ER goes idle (2026-10-04)
**Accept:** the logs show the handshake, heartbeats and a clean timeout.

## P3: One value across, then the loop ✅ (2026-10-04)
- [x] ER: window hidden + focus spoof (60 fps hidden, 2026-10-04). The user presses Continue (no auto-load or warp yet)
- [x] Protocol v2: seqlock state slots `InputState` (Skyrim → ER) and `PlayerState` (ER → Skyrim), torn-read tests in both languages (2026-10-04)
- [x] Skyrim → ER: Sprint (keyboard, Left Shift) is forwarded as Dodge. The hidden ER character backsteps by ER's own rules (2026-10-04)
- [x] ER → Skyrim: PlayerState every frame. Skyrim logs stamina/anim edges and shows an on-screen summary per dodge (2026-10-04)
- [x] Stamina drop visible in Skyrim's log with timestamps (in combat: 136→128; out of combat ER charges no stamina for dodges)
- [x] I-frame window found: ER's dodge flag (TAE FLAG_AS_DODGING) is set for ~27 frames of every roll and no hit lands while it's set. Backsteps have none, so Skyrim sees it once rolls work (2026-10-04, `docs/research/elden-ring-state.md`)
- [x] Roll direction: Skyrim's movement keys (W/A/S/D) steer ER's move stick, so Sprint + direction = roll. Skyrim logs `IFrame on/off` for every
  roll (~450 ms, starting 34–51 ms after the release) and the HUD says "i-frames yes"; Sprint alone still backsteps with none (2026-10-04)
- [x] Coordinate/yaw conversion measured in both games (one walk, W/D/S/A) → `protocol/src/coords.rs` + C++ mirror, unit tests with the
  measured segments (2026-10-04, `docs/research/coordinates.md`)
**Accept:** pressing dodge in Skyrim → ER rolls → Skyrim's log shows the stamina drop and the i-frame window with timestamps. ✔ met 2026-10-04
(stamina drop in combat, i-frame window from rolls).

## P4: Movement & defense (plan: `docs/P4-PLAN.md`)
- [x] Swallow vanilla Skyrim Sprint while the bridge is on (it's the ER dodge now), F10 toggles the bridge. The hidden ER character only
  gets the move stick around a dodge, so it stays parked (2026-10-04)
- [ ] Rolls cost ER stamina while the Skyrim player is in combat (ER makes them free out of combat, so ER is put in its combat state)
- [ ] Skyrim player follows ER's roll movement curve, with Skyrim's collision
- [ ] Rolls play an animation in Skyrim (vanilla only: the sneak "Silent Roll"; no extra animation mods)
- [ ] Skyrim's stamina bar mirrors ER's stamina. Out of stamina = no roll
- [ ] NPC hits during ER i-frames are cancelled
- [ ] **Controller support (last):** PS5 DualSense in Skyrim. Skyrim only reads XInput, so it needs Steam Input, which doesn't apply when SKSE
  starts outside Steam (a plain Steam launch option fails: the loader rejects Steam's extra argument). Options: a `cmd /c start` launch
  option, a Non-Steam-game shortcut, or reading the pad in the plugin itself. Also check that the hidden ER doesn't read the same pad (double input)
**Accept:** the user rolls through an NPC's attack without taking damage, and spamming rolls is limited by stamina.

## P5: Combat bridge
- [ ] Weapon mapping table (Skyrim weapon → ER weapon ID) in `config/`
- [ ] Swallow vanilla attack/block while the bridge is on, and forward them to ER instead
- [ ] Player light/heavy attacks: ER timing + damage math → applied to Skyrim actors (HitResult)
- [ ] NPC → player damage goes through ER defenses. ER HP is the authority. Skyrim HP mirrors it
- [ ] Poise/stagger both ways. Logs show every damage calculation input
**Accept:** damage numbers in the logs match ER's formula. Staggers happen when they should.

## P6: HUD
- [ ] ER-style HP/FP/stamina bars, flask counts, rune counter. Vanilla bars hidden while the bridge is on
**Accept:** the HUD tracks live values with no visible lag.

## P7: Progression
- [ ] Runes from Skyrim kills (scaled by actor level). Rune loss on death + bloodstain (optional)
- [ ] Flasks refill when resting (bed/shrine = "grace"). Leveling through a menu
**Accept:** one full loop: fight → earn runes → rest → level up.

## P8: Visuals & performance (stretch goal)
- [ ] Frame-time profiling on both sides. Budget: 60 fps on the user's PC
- [ ] Optional: ER player/VFX rendered offscreen and depth-composited into Skyrim
- [ ] One-step installer: version checks for both games, SKSE, Address Library and me3, plus an uninstall that leaves the games clean
**Accept:** profiling numbers logged in MODLOG. The user is happy with how it feels.

## Beyond the roadmap: ideas
Not scheduled. These are what could make Skyring feel like its own game rather than "Skyrim with a combat patch". Each one lands only after
P4–P7 work, and only if it can be done the passthrough way (ER calculates, Skyrim shows).

**The world plays by Elden Ring's rules**
- **Standing Stones and shrines as Sites of Grace.** Resting at one refills flasks, lets you level up with runes and respawns nearby enemies.
  The Guardian Stones become the first grace.
- **Runes and bloodstains.** Dying drops your runes where you fell, Skyrim-side. Get back to it without dying again, or they're gone.
- **Dragon Shouts as Ashes of War.** Shouts cost FP instead of a cooldown and use ER's weapon-art timing; Unrelenting Force gets poise damage
  and stance breaks. Words of Power upgrade a Shout like smithing stones upgrade a weapon.
- **Status buildup on Skyrim enemies:** bleed, frost, poison, scarlet rot and sleep as buildup bars, so Skyrim's poisons and enchantments map onto ER effects.

**Bosses worth remembering**
- **Dragons and Dragon Priests as real boss fights:** an ER-style boss health bar, posture/stance breaks into critical hits (Skyrim kill moves as
  the visual), and a second phase at half health. Optional "fog gate" arena lock for named fights.
- **Remembrances:** unique bosses (Alduin, Miraak, Karstaag) drop a Remembrance you trade for a weapon or spell that carries ER-style weapon arts.

**Feel and controls**
- **Lock-on camera** with target switching, which Skyrim lacks, driven by ER's lock-on rules.
- **Guard counters, parries and backstabs**, with ER's frame windows and Skyrim's animations.
- **Spirit Ashes ↔ followers:** summon a Skyrim follower or a conjured creature like a Spirit Ash, which also fixes followers running into
  your rolls.
- **DualSense haptics for free:** once controller support works, the hidden ER already drives adaptive triggers and haptics. Routing that feel
  into Skyrim combat would be a first for a Skyrim mod.

**Built for learning and tuning**
- **"Combat lab" overlay:** an optional on-screen timeline of your last attack or dodge: i-frames, recovery frames, stamina cost, poise damage.
  The data already flows both ways; showing it helps players learn the system and makes bugs easy to report.
- **Per-feature toggles and a difficulty profile** (in-game menu): turn the bridge on and off per system (dodge only, dodge + stamina,
  full ER combat) so it fits any load order or playstyle.
