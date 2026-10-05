#include "bridge/Combat.h"

#include "hooks/PlayerHit.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

// The hit itself is adapted from SkyCraft skse/src/Combat.cpp ApplyHit (MIT, see THIRD-PARTY-NOTICES): a HitData built by Skyrim's own
// ctor + Populate, our damage written in, then Skyrim's apply-hit function (the 38586 that hooks/PlayerHit.cpp byte-checks). Target
// choice, the damage model and the poise build-up are ours. Address Library (AE, all checked with tools/addrlib-check.ps1):
// HitData ctor 43995, HitData::Populate 44001, Actor::DoDamage 37335, Actor::StartCombat 38561, Actor::GetLevel 37334,
// Actor::GetBoundRadius 37439, Actor::IsGhost 37275, ProcessLists 400315, BGSImpactManager 401262 + PlayImpactEffect 36215.
namespace sxer::combat
{
	namespace
	{
		constexpr float kPi = 3.14159265f;
		// Skyrim units: 1 m ~ 70 units. Melee reach = base x the Skyrim weapon's reach + the target's radius.
		constexpr float kBaseReach = 150.0f;
		constexpr float kUnarmedReach = 110.0f;
		constexpr float kMaxHeightDiff = 160.0f;
		constexpr float kHalfCone = 65.0f * kPi / 180.0f;  // in front of the body
		constexpr float kPointBlank = 60.0f;                // this close, any direction counts
		// Poise: an NPC's stance (ER-style, from its level) breaks when the swings' poise damage adds up past it; it recovers after
		// kPoiseResetMs without a hit.
		constexpr std::uint64_t kPoiseResetMs = 3000;

		float g_scale = 1.0f;
		std::uint32_t g_seq = 0;
		bool g_seqSeen = false;
		std::unordered_set<RE::FormID> g_hitThisSwing;
		struct Poise
		{
			float damage = 0;
			std::uint64_t lastMs = 0;
		};
		std::unordered_map<RE::FormID, Poise> g_poise;

		std::uint64_t NowMs() { return GetTickCount64(); }

		using HitDataCtorFn = RE::HitData*(RE::HitData*);

		RE::TESObjectWEAP* EquippedWeapon(RE::PlayerCharacter* a_player)
		{
			auto* right = a_player->GetEquippedObject(false);
			return right ? right->As<RE::TESObjectWEAP>() : nullptr;
		}

		// A vanilla weapon for blood and impact sound when the player holds none (fists): Iron Mace, like SkyCraft's stand-in.
		RE::TESObjectWEAP* ImpactWeapon(RE::TESObjectWEAP* a_held)
		{
			if (a_held && a_held->impactDataSet) {
				return a_held;
			}
			return RE::TESForm::LookupByID<RE::TESObjectWEAP>(0x00013982);
		}

		RE::NiAVObject* HitNode(RE::Actor* a_actor)
		{
			auto* root = a_actor->Get3D();
			if (!root) {
				return nullptr;
			}
			for (const char* name : { "NPC Spine2 [Spn2]", "NPC Spine1 [Spn1]", "NPC Spine [Spn0]", "NPC Pelvis [Pelv]" }) {
				if (auto* node = root->GetObjectByName(name)) {
					return node;
				}
			}
			return root;
		}

		float Stance(float a_level) { return 20.0f + a_level * 0.6f; }

		const char* KindName(std::uint32_t a_kind)
		{
			static constexpr const char* kNames[] = { "none", "light", "heavy", "skill", "other" };
			return a_kind < 5 ? kNames[a_kind] : "?";
		}

		void Land(RE::PlayerCharacter* a_player, RE::Actor* a_target, const proto::PlayerState& a_s, RE::TESObjectWEAP* a_held, std::uint64_t a_frame)
		{
			auto* av = a_target->AsActorValueOwner();
			DamageInput in;
			in.atk[0] = a_s.atk_phys, in.atk[1] = a_s.atk_mag, in.atk[2] = a_s.atk_fire, in.atk[3] = a_s.atk_thun, in.atk[4] = a_s.atk_holy;
			in.level = static_cast<float>(a_target->GetLevel());
			in.armor = av->GetActorValue(RE::ActorValue::kDamageResist);
			in.maxHp = std::max(1.0f, av->GetPermanentActorValue(RE::ActorValue::kHealth));
			in.scale = g_scale;
			const auto r = ComputeDamage(in);
			const float hpBefore = av->GetActorValue(RE::ActorValue::kHealth);

			// Poise (ER-like): accumulate, break past the stance, recover after a pause.
			const auto now = NowMs();
			auto& p = g_poise[a_target->GetFormID()];
			if (now - p.lastMs > kPoiseResetMs) {
				p.damage = 0;
			}
			p.lastMs = now;
			p.damage += a_s.atk_poise;
			const float stance = Stance(in.level);
			const bool broken = p.damage >= stance;
			if (broken) {
				p.damage = 0;
			}
			const bool power = a_s.atk_kind == static_cast<std::uint32_t>(proto::AttackKind::Heavy) ||
			                   a_s.atk_kind == static_cast<std::uint32_t>(proto::AttackKind::Skill);

			auto* weapon = ImpactWeapon(a_held);
			auto* node = HitNode(a_target);
			RE::NiPoint3 hitPos = node ? node->world.translate : a_target->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, 80.0f };
			RE::NiPoint3 dir = hitPos - a_player->GetPosition();
			dir.z = 0.0f;
			dir = dir.Length() > 1e-3f ? dir / dir.Length() : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };

			bool viaPipeline = false;
			if (r.damage > 0.0f && hooks::HitPipelineReady()) {
				alignas(16) std::array<std::byte, sizeof(RE::HitData)> storage{};
				auto* hit = reinterpret_cast<RE::HitData*>(storage.data());
				static REL::Relocation<HitDataCtorFn*> ctor{ REL::ID(43995) };
				ctor(hit);
				hit->Populate(a_player, a_target, nullptr);
				hit->weapon = weapon;
				hit->hitPosition = hitPos;
				hit->hitDirection = dir;
				hit->totalDamage = r.damage;
				hit->physicalDamage = r.damage;
				hit->resistedPhysicalDamage = 0.0f;
				hit->resistedTypedDamage = 0.0f;
				hit->sneakAttackBonus = 1.0f;
				hit->bonusHealthDamageMult = 1.0f;
				hit->stagger = broken ? 1.0f : 0.0f;
				hit->pushBack = 0.0f;
				hit->skill = RE::ActorValue::kNone;  // ER levels the character, not Skyrim's skills
				hit->flags.reset(RE::HitData::Flag::kPowerAttack, RE::HitData::Flag::kCritical, RE::HitData::Flag::kSneakAttack, RE::HitData::Flag::kMeleeAttack);
				hit->flags.set(RE::HitData::Flag::kMeleeAttack);
				if (power) {
					hit->flags.set(RE::HitData::Flag::kPowerAttack);
				}
				viaPipeline = hooks::ApplyHit(a_target, *hit);
			}
			if (!viaPipeline && r.damage > 0.0f) {
				a_target->DoDamage(r.damage, a_player, true);
				if (broken && !a_target->IsDead()) {
					float sdir = (std::atan2(dir.x, dir.y) - a_target->GetAngleZ()) / (2.0f * kPi) + 0.5f;
					sdir -= std::floor(sdir);
					a_target->SetGraphVariableFloat("staggerDirection", sdir);
					a_target->SetGraphVariableFloat("staggerMagnitude", 1.0f);
					a_target->NotifyAnimationGraph("staggerStart");
				}
			}
			if (auto* impacts = RE::BGSImpactManager::GetSingleton(); impacts && weapon && weapon->impactDataSet && node && r.damage > 0.0f) {
				impacts->PlayImpactEffect(a_target, weapon->impactDataSet, node->name.c_str(), dir, 128.0f, false, false);
			}
			if (!a_target->IsDead() && !a_target->IsPlayerTeammate() && !a_target->IsInCombat()) {
				a_target->StartCombat(a_player);
			}
			SKSE::log::info(
				"[combat] hit {} ({:08X}) lvl {:.0f} armor {:.0f} hp {:.0f}->{:.0f}/{:.0f} | ER #{} {} atk phys {:.0f} mag {:.0f} fire {:.0f} ltn {:.0f} holy {:.0f} "
				"| def {:.0f} abs {:.2f} -> ER dmg {:.0f}, ref HP {:.0f}, x{:.2f} -> {:.1f} | poise {:.1f}/{:.0f}{}{} frame={}",
				a_target->GetDisplayFullName(), a_target->GetFormID(), in.level, in.armor, hpBefore, av->GetActorValue(RE::ActorValue::kHealth), in.maxHp,
				a_s.attack_seq, KindName(a_s.atk_kind), in.atk[0], in.atk[1], in.atk[2], in.atk[3], in.atk[4], r.defense, r.absorption, r.erDamage,
				r.refHp, g_scale, r.damage, broken ? stance : p.damage, stance, broken ? " STAGGER" : "", viaPipeline ? "" : " (fallback DoDamage)", a_frame);
		}

		// Actors the swing reaches: alive, loaded, not the player or a follower, within reach and in front of the body.
		void Sweep(RE::PlayerCharacter* a_player, const proto::PlayerState& a_s, float a_facing, std::uint64_t a_frame)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			auto* held = EquippedWeapon(a_player);
			const float reach = held ? kBaseReach * std::max(0.5f, held->GetReach()) : kUnarmedReach;
			const auto me = a_player->GetPosition();
			const RE::NiPoint3 fwd{ std::sin(a_facing), std::cos(a_facing), 0.0f };
			for (auto& handle : lists->highActorHandles) {
				auto target = handle.get();
				auto* actor = target.get();
				if (!actor || actor == a_player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded() || actor->IsGhost() ||
					actor->IsPlayerTeammate() || g_hitThisSwing.contains(actor->GetFormID())) {
					continue;
				}
				auto d = actor->GetPosition() - me;
				if (std::fabs(d.z) > kMaxHeightDiff) {
					continue;
				}
				d.z = 0.0f;
				const float dist = d.Length();
				if (dist > reach + actor->GetBoundRadius()) {
					continue;
				}
				if (dist > kPointBlank) {
					const float cosAngle = (d.x * fwd.x + d.y * fwd.y) / dist;
					if (cosAngle < std::cos(kHalfCone)) {
						continue;
					}
				}
				g_hitThisSwing.insert(actor->GetFormID());
				Land(a_player, actor, a_s, held, a_frame);
			}
		}
	}

	void LoadConfig()
	{
		// The ini sits next to the DLL (Data/SKSE/Plugins/SkyrimXER.ini, deployed from config/SkyrimXER.ini).
		wchar_t module[MAX_PATH]{};
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&LoadConfig), &self);
		GetModuleFileNameW(self, module, MAX_PATH);
		std::wstring ini(module);
		ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"SkyrimXER.ini";
		wchar_t value[64]{};
		GetPrivateProfileStringW(L"Combat", L"fDamageScale", L"1.0", value, 64, ini.c_str());
		try {
			g_scale = std::clamp(std::stof(value), 0.01f, 100.0f);
		} catch (...) {
			g_scale = 1.0f;
		}
		SKSE::log::info("[combat] fDamageScale = {:.2f} (SkyrimXER.ini [Combat], default 1)", g_scale);
	}

	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, float a_facing, std::uint64_t a_frame)
	{
		if (!a_player || !a_state) {
			return;
		}
		const auto& s = *a_state;
		if (!g_seqSeen || s.attack_seq != g_seq) {
			const bool first = !g_seqSeen;
			g_seqSeen = true;
			g_seq = s.attack_seq;
			g_hitThisSwing.clear();
			if (first) {
				return;  // the swing that was already over when we connected
			}
			// A window opened since the last frame (even if it already closed again: 2-3 ER frames): sweep once now.
			Sweep(a_player, s, a_facing, a_frame);
			return;
		}
		// Still inside the same window: targets stepping in are hit too (once each).
		if (s.flags & (1u << static_cast<std::uint32_t>(proto::PlayerFlag::AttackActive))) {
			Sweep(a_player, s, a_facing, a_frame);
		}
	}
}
