#include "hooks/PlayerHit.h"

#include <atomic>
#include <cstring>

// Skyrim's melee HitFrame handler (AE id 38627) builds a HitData and, at +0x4A8, calls AE id 38586, which applies the hit to the victim:
// damage, block, hit reaction and stagger, pain voice, the TESHitEvent, kill credit. That call is replaced: on the player during ER's roll
// i-frames the hit is dropped whole (ER owns i-frames, DESIGN §3); everything else goes to 38586 unchanged. The call site is trusted only
// if it really is `call 38586` on this build, else nothing is patched.
// Adapted from SkyCraft skse/src/Combat.cpp ResolveHitPipeline (MIT, see THIRD-PARTY-NOTICES). Ids checked with tools/addrlib-check.ps1.
namespace sxer::hooks
{
	namespace
	{
		std::atomic<bool> iframe{ false };

		struct ApplyHitHook
		{
			static void thunk(RE::Actor* a_victim, RE::HitData& a_hit)
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!a_victim || a_victim != player) {
					func(a_victim, a_hit);
					return;
				}
				const auto attacker = a_hit.aggressor.get();
				const auto id = attacker ? attacker->GetFormID() : 0u;
				const char* name = attacker ? attacker->GetName() : "?";
				const bool skip = iframe.load(std::memory_order_relaxed);
				auto* av = player->AsActorValueOwner();
				const float before = av->GetActorValue(RE::ActorValue::kHealth);
				if (skip) {
					SKSE::log::info("[hit] skipped (ER i-frames): {:08X} {} damage {:.1f} health {:.1f}", id, name, a_hit.totalDamage, before);
					return;
				}
				func(a_victim, a_hit);
				SKSE::log::info("[hit] landed: {:08X} {} damage {:.1f} health {:.1f} -> {:.1f}", id, name, a_hit.totalDamage, before,
					av->GetActorValue(RE::ActorValue::kHealth));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void SetPlayerIFrame(bool a_on) { iframe.store(a_on, std::memory_order_relaxed); }

	void InstallPlayerHit()
	{
		const auto site = REL::ID(38627).address() + 0x4A8;
		const auto target = REL::ID(38586).address();
		const auto* code = reinterpret_cast<const std::uint8_t*>(site);
		std::int32_t rel = 0;
		std::memcpy(&rel, code + 1, 4);
		if (code[0] != 0xE8 || site + 5 + rel != target) {
			SKSE::log::warn("[hit] melee call site 38627+0x4A8 isn't `call 38586` on this build (byte {:02X}); i-frames won't protect the player", code[0]);
			return;
		}
		ApplyHitHook::func = SKSE::GetTrampoline().write_call<5>(site, ApplyHitHook::thunk);
		SKSE::log::info("[hit] hook installed: melee hits on the player are dropped during ER i-frames (38627+0x4A8 -> 38586)");
	}
}
