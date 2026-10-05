#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace RE
{
	class PlayerCharacter;
	class TESObjectWEAP;
}

// P5 step 2: the Skyrim weapon in the right hand picks the ER weapon ER swings (its moveset and attack rating). Pure part (selftest):
// kinds, the default ER ids, and the upgrade level from the Skyrim weapon's material tier. WeaponMap.cpp reads the Skyrim weapon and
// SkyrimXER.ini [Weapons].
namespace sxer::weapons
{
	enum class Kind : std::uint32_t
	{
		kNone,  // no weapon (spells, bows, staves, sheathed): ER keeps its own
		kDagger,
		kSword,
		kWarAxe,
		kMace,
		kGreatsword,
		kBattleaxe,
		kWarhammer,
		kCount
	};

	inline const char* Name(Kind a_kind)
	{
		static constexpr const char* kNames[] = { "none", "dagger", "sword", "war axe", "mace", "greatsword", "battleaxe", "warhammer" };
		return kNames[std::min<std::uint32_t>(static_cast<std::uint32_t>(a_kind), 7)];
	}

	// ER weapon param ids per kind (+0): Dagger, Longsword, Bastard Sword, Hand Axe, Mace, Greataxe, Large Club. Overridable in
	// SkyrimXER.ini [Weapons] (keys = the kind names without spaces: Dagger, Sword, WarAxe, Mace, Greatsword, Battleaxe, Warhammer).
	using Table = std::array<std::int32_t, static_cast<std::size_t>(Kind::kCount)>;
	inline constexpr Table kDefaultIds{ 0, 1000000, 2000000, 14000000, 11000000, 3000000, 15000000, 12000000 };

	// Damage of the iron weapon of each kind (vanilla Skyrim.esm), the bottom of its material ladder.
	inline constexpr std::array<float, static_cast<std::size_t>(Kind::kCount)> kIronDamage{ 0, 4, 7, 8, 9, 15, 16, 18 };

	// ER upgrade level from Skyrim's material tier: ~1 damage per tier above iron (2H a bit more), 3 levels per point, capped at +25
	// (iron +0, steel +3, ... daedric ~+21, dragonbone +24/+25).
	inline int UpgradeLevel(Kind a_kind, float a_damage)
	{
		if (a_kind == Kind::kNone) {
			return 0;
		}
		const float above = a_damage - kIronDamage[static_cast<std::size_t>(a_kind)];
		return std::clamp(static_cast<int>(std::lround(above * 3.0f)), 0, 25);
	}

	// The ER weapon id with its upgrade level (ER ids: base + level), 0 = none.
	inline std::int32_t ErWeapon(Kind a_kind, float a_damage, const Table& a_table)
	{
		const auto base = a_table[static_cast<std::size_t>(a_kind)];
		return base > 0 ? base + UpgradeLevel(a_kind, a_damage) : 0;
	}

	// Game side (WeaponMap.cpp): Skyrim weapon → kind; SkyrimXER.ini [Weapons] (kDataLoaded); the ER weapon for the player's
	// right-hand weapon (0 = none), with its kind.
	Kind Classify(const RE::TESObjectWEAP* a_weapon);
	void LoadTable();
	std::int32_t ForPlayer(RE::PlayerCharacter* a_player, Kind* a_kind);
}
