#pragma once

#include <algorithm>

// P5 step 1: the ER-like damage model, pure (no game headers) so the selftest can check it. bridge/Combat.cpp applies it.
namespace sxer::combat
{
	// One swing on one target: ER's defense curve per damage type against a defense built from the target's level, armour absorption
	// from its armour rating, then scaled by its max HP / an ER enemy's HP at that level, so it takes about as many hits as a
	// comparable ER enemy (user choice 2026-10-05), x fDamageScale.
	struct DamageInput
	{
		float atk[5]{};  // phys, magic, fire, lightning, holy (ER, before defense)
		float level = 1;
		float armor = 0;  // Skyrim armour rating (DamageResist)
		float maxHp = 100;
		float scale = 1;  // fDamageScale
	};
	struct DamageResult
	{
		float defense = 0;
		float absorption = 0;
		float erDamage = 0;  // after ER's defense curve and absorption
		float refHp = 0;     // HP of an ER enemy of this level
		float damage = 0;    // Skyrim HP to take away
	};

	// ER's attack/defense curve (community-documented piecewise form): the share of an attack that gets through a defense, 0.1..0.9.
	inline float DefenseCurve(float a_attack, float a_defense)
	{
		if (a_attack <= 0.0f) {
			return 0.0f;
		}
		const float r = a_attack / std::max(1.0f, a_defense);
		if (r > 8.0f) {
			return 0.9f;
		}
		if (r > 2.5f) {
			const float t = (r - 2.5f) / 5.5f;
			return 0.7f + 0.2f * t * t;
		}
		if (r > 1.0f) {
			const float t = (2.5f - r) / 1.5f;
			return 0.7f - 0.3f * t * t;
		}
		if (r > 0.125f) {
			const float t = (r - 0.125f) / 0.875f;
			return 0.1f + 0.3f * t * t;
		}
		return 0.1f;
	}

	inline DamageResult ComputeDamage(const DamageInput& a_in)
	{
		DamageResult r;
		const float level = std::clamp(a_in.level, 1.0f, 150.0f);
		// An ER enemy of comparable level: defense grows with the area's difficulty, HP faster (an early soldier ~ 150-250 HP / ~100
		// defense, late game ~ 1600+ HP / ~300 defense).
		r.defense = 100.0f + 4.0f * level;
		r.absorption = std::clamp(a_in.armor / 1000.0f, 0.0f, 0.5f);
		for (float atk : a_in.atk) {
			r.erDamage += atk * DefenseCurve(atk, r.defense) * (1.0f - r.absorption);
		}
		r.refHp = 120.0f + 30.0f * level;
		r.damage = r.erDamage * (a_in.maxHp / r.refHp) * a_in.scale;
		return r;
	}
}
