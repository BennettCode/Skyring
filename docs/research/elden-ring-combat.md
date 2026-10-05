# Elden Ring combat state (P5)

## The swing's hit window (2026-10-05)
- **Signal:** `CSChrActionFlagModule.action_modifiers_flags` bit 9 (`invokeknockbackvalue`) and bit 10 (`parryable`), plus the byte at
  module **+0x1d0** (reads 6), all set together for **2 frames** of a light attack and **3 frames** of a heavy release at 60 fps, in every
  swing measured (6 single R1, a 6-hit chain, 5 charged R2, 3 weapon arts, colossal sword one-handed; greatsword two-handed in the live
  test). They sit inside the fastest part of the sweep: right-hand model-space speed 21–29 units/s against ~2 at rest.
- Two-handed swings open **two** windows ~0.25 s apart in one animation play (e.g. 24032000 at t≈0.45 and 0.70): count them as one swing.
- **Not it:** action_flag +0x84 (f32 −1 → 720 at the top of the wind-up, hand still: a turn-speed value), bit 32 (`disable_direction_change`,
  wind-up), bit 36 (`ai_parry_signal`, just before the swing: what AI uses to react), physics +0x120 (root-motion lunge velocity).
- **Method:** `-ErDump` (combat.rs Dump, every frame during attack anims, untyped regions behind time_act's `chr_tae_anim_event` and the
  container's `hitstop`/`damage` pointers read only after VirtualQuery) + a bit-window analysis; then a full-rate per-frame text probe
  (the 18 KB/frame dump slowed ER to 14 fps). Gotcha: `ChrIns.modules` is an `OwnedPtr` field: the container is where it points (reading
  the field's own address as the container crashed ER).
- **No AtkParam row id** exists in the player's typed modules, PlayerIns or the three untyped regions during the window. Exact rows would
  need a hook on `FieldInsBaseVmt::get_atk_param_for_behavior` (eldenring-rs `field_ins.rs:99-126`).

## Attack kinds (anim id mod 1 000 000, any stance prefix)
- 300xx one-handed R1 chain, 320xx two-handed R1; 305xx / 325xx R2 (…505 = release after a charge); 40xxx weapon art; other 3xxxx =
  running/rolling/jumping/backstep attacks.

## Attack rating from params (attack.rs)
- Weapon = `equipment_param_ids[active_weapon_slot(Right)]` (upgrade level = id mod 100; param row = id rounded down to 100).
- Per type: base = attack_base_* x ReinforceParamWeapon *_atk_rate (row = reinforce_type_id + level); bonus per stat allowed by
  AttackElementCorrectParam = base x correct_<stat> x correct_<stat>_rate / 100 x CalcCorrectGraph(correct_type_*, stat) / 100.
  Two-handing: strength x1.5 (cap 148). Poise = sa_weapon_damage x sa_weapon_atk_rate.
- Example (test character, str 55): fists 110000 → phys 29; weapon 10050000 two-handed → phys 88, fire 75. **Not yet checked against
  ER's own status screen.**

## Equipping by id (P5 step 2, 2026-10-05)
- `ChrAsm.equipment_param_ids[1|3|5]` (right-hand slots) written in **both** copies (PlayerIns.chr_asm and PlayerGameData.equipment.chr_asm)
  is enough: the next swing uses that weapon's moveset (anim group prefix: dagger 20, straight sword 23, greatsword 25, axe 30,
  greataxe 32, hammer 33, great hammer 35, colossal sword 42, twinblade 24) and attack.rs's attack rating follows. The hidden model may
  not change (not needed). No inventory item, gaitem handle or equip call. 7 classes + a +21 upgrade, no crash.
- Pitfall: picking the stance slot by the *current* ids after writing ours made the slot flip every frame; pick from the save's ids.
- AR at str 55 / dex 15 (+0 one-handed): Dagger 102, Longsword 156 (+3: 187, +21: 390), Hand Axe 180, Mace 168; two-handed: Bastard
  Sword 214, Greataxe 236, Large Club 219.

## Bows and ammo (2026-10-05)
- A bow id written like a melee weapon (Longbow 41000000) loads and draws, but with only the ammo **param id** in the Arrow slot ER
  plays 50050 (reach for the quiver, nothing there, 2.6 s, regardless of the button). ER fires only **owned** ammo **equipped the full
  way**: Arrow1 = slot 6 / Bolt1 = slot 7 need the param id + gaitem handle in both ChrAsm copies, `equipment_item_idx_list[slot]` =
  key-items capacity (384) + the entry's position in the normal inventory list (matched 4/4 equipped items of the save), and
  `equipment_entries.arrow_primary`/`bolt_primary` = the entry's ItemId.
- With that: R1 → x36010 (draw) → x36020 (hold at full draw while held) → **x36000 on release** → bow idle 14000000 (longbow,
  two-handed: prefix 44). No hit-window flags fire for bows; the switch into x36000 is the shot.
- The test character owns arrows 50000000 x20, 50010000 x10 and several bolts; the save had no arrows equipped (slot 6 = -1).
