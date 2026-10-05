#include "bridge/Health.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

// Adapted from SkyCraft skse/src/Combat.cpp (MIT, see THIRD-PARTY-NOTICES): BridgePlayerDamage (refund every health drop and forward
// it), HitSink (who hit, how), SetEssential / KillPlayer. The mirror of ER's HP onto the bar is our Stamina.cpp pattern (FalloutCraft).
// Address Library (AE): Actor::KillImmediate 37735, Actor::GetActorValueMax (GetActorValueModifier 38469); KillImpl is vfunc 0x110.
namespace sxer::health
{
	namespace
	{
		constexpr std::uint32_t kInWorld = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::InWorld);
		constexpr std::uint32_t kIFrame = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::IFrame);
		constexpr std::uint32_t kDowned = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::Downed);
		constexpr float kMinChange = 0.05f;  // smaller moves are rounding, not a hit
		constexpr std::uint64_t kHitMemoryMs = 1500;

		struct State
		{
			bool active = false;
			bool primed = false;
			bool essential = false;
			float expected = 0;  // the health we last set
			double hurtTotal = 0;
			std::uint32_t respawn = 0;
			bool killed = false;
			int lastLoggedHp = -1;
		} g;

		// The last TESHitEvent on the player (main thread writes and reads).
		struct LastHit
		{
			RE::FormID attacker = 0;
			const char* kind = "other";
			bool power = false;
			bool blocked = false;
			std::uint64_t atMs = 0;
		} g_hit;

		std::uint64_t NowMs() { return static_cast<std::uint64_t>(GetTickCount64()); }

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			static HitSink* Get()
			{
				static HitSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!a_event || !player || a_event->target.get() != player) {
					return RE::BSEventNotifyControl::kContinue;
				}
				LastHit hit;
				hit.attacker = a_event->cause ? a_event->cause->GetFormID() : 0;
				hit.atMs = NowMs();
				if (a_event->projectile != 0) {
					hit.kind = "projectile";
				} else if (auto* source = RE::TESForm::LookupByID(a_event->source)) {
					switch (source->GetFormType()) {
					case RE::FormType::Spell:
					case RE::FormType::Enchantment:
					case RE::FormType::Scroll:
					case RE::FormType::Ingredient:
					case RE::FormType::AlchemyItem:
						hit.kind = "magic";
						break;
					default:
						hit.kind = "melee";
						break;
					}
				}
				hit.power = a_event->flags.any(RE::TESHitEvent::Flag::kPowerAttack);
				hit.blocked = a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
				g_hit = hit;
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void SetEssential(RE::PlayerCharacter* a_player, bool a_on)
		{
			if (a_on == g.essential) {
				return;
			}
			auto& flags = a_player->GetActorRuntimeData().boolFlags;
			if (a_on) {
				flags.set(RE::Actor::BOOL_FLAGS::kEssential);
			} else {
				flags.reset(RE::Actor::BOOL_FLAGS::kEssential);
			}
			g.essential = a_on;
			SKSE::log::info("[health] player essential {} (ER's HP decides death)", a_on ? "on" : "off");
		}

		void Kill(RE::PlayerCharacter* a_player, std::uint64_t a_frame)
		{
			SetEssential(a_player, false);
			const bool recent = NowMs() - g_hit.atMs < 5000;
			auto* killer = recent && g_hit.attacker ? RE::TESForm::LookupByID<RE::Actor>(g_hit.attacker) : nullptr;
			const float hp = a_player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			SKSE::log::info("[health] ER HP ran out (Downed): killing the Skyrim player (killer {:08X}) frame={}", killer ? killer->GetFormID() : 0u, a_frame);
			a_player->KillImpl(killer, hp + 1.0f, true, false);
			if (!a_player->IsDead()) {
				a_player->KillImmediate();
			}
		}
	}

	void Install()
	{
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESHitEvent>(HitSink::Get());
		}
	}

	void OnLoad()
	{
		++g.respawn;
		g.primed = false;
		g.killed = false;
		SKSE::log::info("[health] save loaded: respawn #{} (ER refills its HP)", g.respawn);
	}

	double HurtTotal() { return g.hurtTotal; }
	std::uint32_t RespawnSeq() { return g.respawn; }

	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, std::uint64_t a_frame)
	{
		const bool use = a_player && a_state && (a_state->flags & kInWorld) && a_state->max_hp > 0 && !a_player->IsDead();
		if (use != g.active) {
			g.active = use;
			g.primed = false;
			SKSE::log::info("[health] {} frame={}", use ? "on: Skyrim's health is ER's HP" : "off: Skyrim's own health", a_frame);
			if (!use && a_player) {
				SetEssential(a_player, false);
			}
		}
		if (!use) {
			return;
		}
		SetEssential(a_player, true);
		if ((a_state->flags & kDowned) && !g.killed) {
			g.killed = true;
			Kill(a_player, a_frame);
			return;
		}
		auto* owner = a_player->AsActorValueOwner();
		const float max = a_player->GetActorValueMax(RE::ActorValue::kHealth);
		if (max <= 0) {
			return;
		}
		const float cur = owner->GetActorValue(RE::ActorValue::kHealth);
		if (!g.primed) {
			g.primed = true;  // whatever the save or the last session left isn't a new hit
		} else if (const float change = g.expected - cur; std::fabs(change) > kMinChange) {
			const bool hit = change > 0;
			const bool recent = NowMs() - g_hit.atMs < kHitMemoryMs;
			if (hit && (a_state->flags & kIFrame)) {
				SKSE::log::info("[health] {:.1f} damage blocked by ER i-frames ({} from {:08X}) frame={}", change, recent ? g_hit.kind : "unknown",
					recent ? g_hit.attacker : 0u, a_frame);
			} else {
				g.hurtTotal += change / max;
				if (hit) {
					SKSE::log::info("[health] hit: {:.1f} of {:.0f} Skyrim health ({:.1f}%) {} from {:08X}{}{} -> ER frame={}", change, max, 100.0f * change / max,
						recent ? g_hit.kind : "non-hit damage", recent ? g_hit.attacker : 0u, recent && g_hit.power ? ", power attack" : "",
						recent && g_hit.blocked ? ", blocked" : "", a_frame);
					g_hit.atMs = 0;  // one hit event explains one drop
				} else if (-change > 1.0f) {
					SKSE::log::info("[health] healed {:.1f} Skyrim health -> ER frame={}", -change, a_frame);
				}
			}
		}
		// Skyrim's bar = ER's HP share (damage-modifier move, read back).
		const float target = max * std::clamp(static_cast<float>(a_state->hp) / static_cast<float>(a_state->max_hp), 0.0f, 1.0f);
		const float delta = target - owner->GetActorValue(RE::ActorValue::kHealth);
		if (std::fabs(delta) > kMinChange) {
			owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, delta);
		}
		g.expected = owner->GetActorValue(RE::ActorValue::kHealth);
		if (a_state->hp != g.lastLoggedHp && (g.lastLoggedHp < 0 || std::abs(a_state->hp - g.lastLoggedHp) >= a_state->max_hp / 50)) {
			g.lastLoggedHp = a_state->hp;
			SKSE::log::info("[health] ER HP {}/{} -> Skyrim {:.0f}/{:.0f} frame={}", a_state->hp, a_state->max_hp, g.expected, max, a_frame);
		}
	}
}
