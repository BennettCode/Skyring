# P5 plan: Elden Ring combat in Skyrim

User goal (2026-10-05): Skyrim should feel like playing Elden Ring with a controller: ER's buttons (done in P4 step 8), ER's swings and
movesets, ER's damage. Authority (DESIGN §3): **ER calculates, Skyrim applies.** Skyrim knows *what* is hit, ER knows *when* and *how much*.

## Step 1: ER swings damage Skyrim NPCs (done 2026-10-05)
- **Stage A, probe** (findings: `docs/research/elden-ring-combat.md`): eldenring-rs has no hitbox or damage module. A per-frame memory
  dump and a full-rate text probe found the **hit window**: `action_modifiers_flags` bit 9/10 or action_flag +0x1d0, 2–3 frames in
  the fastest part of every sweep. No AtkParam row id is readable, so the motion value comes from the attack kind (anim group).
- **Stage B:** protocol **v9** (PlayerState `attack_seq`, `atk_phys/mag/fire/thun/holy`, `atk_poise`, `atk_kind`, PlayerFlag
  `AttackActive`). ER `er-plugin/src/attack.rs`: attack rating from ER's params (EquipParamWeapon, ReinforceParamWeapon,
  AttackElementCorrectParam, CalcCorrectGraph, the character's stats) x motion value; one swing per animation play.
  Skyrim `skse/src/bridge/Combat.cpp`: targets in reach and in front of the body the player sees, ER-like damage
  (`CombatMath.h`: ER defense curve vs a defense from the NPC's level, armour absorption, x NPC max HP / ER enemy HP at that level,
  x `fDamageScale` in `SkyrimXER.ini`), poise build-up → stagger, applied through Skyrim's own hit function (SkyCraft's path).
- **User choice:** ER-like damage (enemies take about as many hits as a comparable ER enemy), one tuning number.

## Step 2: the Skyrim weapon picks the ER weapon (done 2026-10-05)
- **Probe:** writing an ER weapon id into the right-hand slots of both ChrAsm copies (no equip call) switches ER's moveset and attack
  rating; no crash over 7 weapon classes (`docs/research/elden-ring-combat.md`).
- **Protocol v10:** InputState `er_weapon`. Skyrim `bridge/WeaponMap.{h,cpp}`: dagger → Dagger, sword → Longsword, war axe → Hand Axe,
  mace → Mace, greatsword → Bastard Sword, battleaxe → Greataxe, warhammer (keyword WeapTypeWarhammer) → Large Club; ids in
  `SkyrimXER.ini [Weapons]`; upgrade level from the Skyrim material (≈3 levels per damage point above iron, cap +25).
  ER `stance.rs` writes it into the right slot the stance uses (never mid-swing/dodge; slot choice from the save's own ids), and puts the
  save's ids back when the bridge lets go.

## Step 3: enemy hits on you go to your ER HP (done 2026-10-05)
- User choices: Skyrim's armour decides the amount (same share of health as in Skyrim, applied to ER HP); ER HP 0 = Skyrim death, the
  hidden ER character held at 1 HP and refilled when Skyrim loads a save.
- **Protocol v11:** InputState `hurt_total` (f64 running share) + `respawn_seq`; PlayerFlag `Downed`.
- Skyrim `bridge/Health.cpp` (SkyCraft BridgePlayerDamage/HitSink/KillPlayer pattern): every health change of the player (any source)
  is refunded and forwarded; the bar shows ER's HP share; ER i-frames block damage of any kind; Downed → KillImpl/KillImmediate.
  ER `er-plugin/src/health.rs`: applies the share to ER HP (sub-HP remainders carried), keeps the baseline across Skyrim pauses.
- Found: the bool essential flag doesn't stop a hit bigger than the player's remaining health from killing in Skyrim directly. That
  outcome matches anyway (the bar equals ER's share, so ER would reach 0 too); the reload refills ER.

## Next steps
4. Later: exact AtkParam rows (hook ER's attack-param lookup), status buildup, guard counters, ranged/magic.
