#include "bridge/WeaponMap.h"

#include <Windows.h>

#include <string>

namespace sxer::weapons
{
	namespace
	{
		Table g_table = kDefaultIds;

		std::wstring IniPath()
		{
			wchar_t module[MAX_PATH]{};
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&IniPath), &self);
			GetModuleFileNameW(self, module, MAX_PATH);
			std::wstring ini(module);
			return ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SkyrimXER.ini";
		}
	}

	// Skyrim weapon → kind. Battleaxes and warhammers share kTwoHandAxe; the WeapTypeWarhammer keyword tells them apart.
	Kind Classify(const RE::TESObjectWEAP* a_weapon)
	{
		if (!a_weapon) {
			return Kind::kNone;
		}
		using Type = RE::WEAPON_TYPE;
		switch (a_weapon->GetWeaponType()) {
		case Type::kOneHandDagger: return Kind::kDagger;
		case Type::kOneHandSword: return Kind::kSword;
		case Type::kOneHandAxe: return Kind::kWarAxe;
		case Type::kOneHandMace: return Kind::kMace;
		case Type::kTwoHandSword: return Kind::kGreatsword;
		case Type::kTwoHandAxe: return a_weapon->HasKeywordString("WeapTypeWarhammer") ? Kind::kWarhammer : Kind::kBattleaxe;
		default: return Kind::kNone;
		}
	}

	void LoadTable()
	{
		const auto ini = IniPath();
		static constexpr const wchar_t* kKeys[] = { L"", L"Dagger", L"Sword", L"WarAxe", L"Mace", L"Greatsword", L"Battleaxe", L"Warhammer" };
		for (std::size_t k = 1; k < g_table.size(); ++k) {
			g_table[k] = static_cast<std::int32_t>(GetPrivateProfileIntW(L"Weapons", kKeys[k], kDefaultIds[k], ini.c_str()));
		}
		SKSE::log::info("[weapons] ER weapons: dagger {} sword {} war axe {} mace {} greatsword {} battleaxe {} warhammer {} (SkyrimXER.ini [Weapons])",
			g_table[1], g_table[2], g_table[3], g_table[4], g_table[5], g_table[6], g_table[7]);
	}

	std::int32_t ForPlayer(RE::PlayerCharacter* a_player, Kind* a_kind)
	{
		auto* form = a_player ? a_player->GetEquippedObject(false) : nullptr;
		auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
		const auto kind = Classify(weapon);
		if (a_kind) {
			*a_kind = kind;
		}
		return weapon ? ErWeapon(kind, static_cast<float>(weapon->GetAttackDamage()), g_table) : 0;
	}
}
