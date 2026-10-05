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

## Next steps
2. **Weapon types:** Skyrim weapon type → ER weapon (table in `config/`, equipped like `stance.rs`), so a dagger, sword, axe, mace,
   greatsword, battleaxe or warhammer swings with ER's matching moveset and attack rating.
3. **NPC hits on the player go to ER:** ER's HP and defenses decide, Skyrim's HP bar mirrors ER's (`hooks/PlayerHit.cpp` already sees
   every melee hit on the player).
4. Later: exact AtkParam rows (hook ER's attack-param lookup), status buildup, guard counters, ranged/magic.
