#include "bridge/Stamina.h"

#include <algorithm>
#include <cmath>

// Fraction mirror adapted from FalloutCraft FO4_ModFiles/fo_combat.cpp:151-188 (MIT, see THIRD-PARTY-NOTICES): the host value is set to the
// hidden game's share of the host maximum by moving the damage modifier, then read back to check it took.
// Calls: ActorValueOwner::GetActorValue / GetPermanentActorValue / ModActorValue are vfuncs; Actor::GetActorValueMax uses
// GetActorValueModifier = AE id 38469 (checked with tools/addrlib-check.ps1).
namespace sxer::stamina
{
	namespace
	{
		constexpr std::uint32_t kInWorld = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::InWorld);
		constexpr float kMinDelta = 0.05f;  // smaller differences are rounding, not a change
		constexpr int kLogStep = 10;        // log ER stamina when it moved this much since the last line (or hit 0 / full)

		struct State
		{
			bool active = false;
			int lastLogged = -1;
			int missLogs = 0;
		} g;
	}

	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, std::uint64_t a_frame)
	{
		const bool use = a_player && a_state && (a_state->flags & kInWorld) && a_state->max_stamina > 0 && !a_player->IsDead();
		if (use != g.active) {
			g.active = use;
			g.lastLogged = -1;
			SKSE::log::info("[stamina] mirror {} frame={}", use ? "on (Skyrim's bar shows ER's stamina)" : "off (Skyrim's own stamina)", a_frame);
		}
		if (!use) {
			return;
		}
		auto* owner = a_player->AsActorValueOwner();
		const float max = a_player->GetActorValueMax(RE::ActorValue::kStamina);
		if (max <= 0) {
			return;
		}
		const float frac = std::clamp(static_cast<float>(a_state->stamina) / static_cast<float>(a_state->max_stamina), 0.0f, 1.0f);
		const float target = max * frac;
		const float delta = target - owner->GetActorValue(RE::ActorValue::kStamina);
		if (std::fabs(delta) > kMinDelta) {
			owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, delta);
			const float left = target - owner->GetActorValue(RE::ActorValue::kStamina);
			if (std::fabs(left) > 1.0f && g.missLogs < 5) {
				++g.missLogs;
				SKSE::log::warn("[stamina] damage modifier didn't take: target {:.1f}, {:.1f} off after a {:.1f} change frame={}", target, left, delta, a_frame);
			}
		}
		const int er = a_state->stamina;
		if (g.lastLogged < 0 || std::abs(er - g.lastLogged) >= kLogStep || (er != g.lastLogged && (er == 0 || er == a_state->max_stamina))) {
			g.lastLogged = er;
			SKSE::log::info("[stamina] ER {}/{} -> Skyrim {:.0f}/{:.0f} frame={}", er, a_state->max_stamina, owner->GetActorValue(RE::ActorValue::kStamina), max, a_frame);
		}
	}
}
