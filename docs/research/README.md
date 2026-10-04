# docs/research: reverse-engineering notes

**Notes only.** Never paste decompiler output, disassembly listings, or dumped game data here. Describe what something does in
your own words: names, offsets/IDs, field meanings, how you found it, and which game version it applies to.

Suggested files (create as needed):
- `skyrim-hooks.md`: Address Library IDs (SE/AE), what each hook intercepts, vtable indices
- `skyrim-combat.md`: hit pipeline, actor values, animation graph events/variables
- `elden-ring-structs.md`: player/char structs, fields (stamina, FP, HP, poise, action state), eldenring-rs type names
- `elden-ring-input.md`: how input reaches the player, how to inject it while unfocused
- `elden-ring-params.md`: param tables + IDs used (AttackParam_Pc, EquipParamWeapon, SpEffectParam, ...)
- `coordinates.md`: axis/handedness/yaw test results (P3, measured)

Note template:
```
## <Thing>
- Game/version: SkyrimSE 1.7.104 | eldenring.exe 2.7.1.0
- Location: Address Library ID ____ / RVA ____ / eldenring-rs `path::Type`
- What it does: ...
- How found: ...
- Confidence: guessed / observed once / verified by test (link to MODLOG entry)
```
