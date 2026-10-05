#include "bridge/Locomotion.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include "bridge/Coords.h"
#include "bridge/Movement.h"
#include "hooks/ControllerVelocity.h"

// Suspend/yield checks use CommonLib members; the ones that call into the game: Actor::IsInMidair = AE id 37243, Actor::IsBlocking =
// AE id 37952 (both checked with tools/addrlib-check.ps1). The rest read ActorState bits or vfuncs.
namespace sxer::locomotion
{
	namespace
	{
		constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
		constexpr std::uint32_t kInWorld = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::InWorld);
		// Larger ER jumps between two frames than this are teleports/reloads, never locomotion (park.rs MAX_STEP_M).
		constexpr float kMaxStepM = 1.5f;
		// Skyrim's walk toggle (Caps Lock, PlayerControls running = false) sends this stick size: ER walks at 0.6, does nothing at 0.3
		// (docs/research/elden-ring-input.md "Locomotion probe").
		constexpr float kWalkStick = 0.6f;
		// In the air this many frames in a row = a fall (vanilla); shorter = steps, porches and bumps, ER keeps driving (a 6-frame limit
		// broke a sprint off a porch, 2026-10-05). A jump (Skyrim's bInJumpState) is vanilla at once.
		constexpr int kMidairFrames = 30;
		// Summary line every kSummaryFrames frames while ER moved.
		constexpr int kSummaryFrames = 120;
		// Facing error is only counted once the keys have been held this long (ER turns in ~10 frames, plus the timeline's delay).
		constexpr int kTurnInFrames = 20;

		struct State
		{
			bool running = false;
			bool suspended = false;
			const char* why = nullptr;
			int midair = 0;
			float w = 0;
			float facing = 0;
			float camYaw = 0;
			RE::NiPoint3 target;
			bool haveLast = false;
			std::array<float, 3> lastEr{};
			std::int32_t anim = -1;
			int steerFrames = 0;
			// Summary window.
			int frames = 0;
			int stalls = 0;
			float erDistM = 0;
			float skyDist = 0;
			float maxFacingErr = 0;
			RE::NiPoint3 lastHere;
		} g;

		bool EnvEnabled()
		{
			static const bool enabled = [] {
				const char* v = std::getenv("SKYRIMXER_LOCO");
				return !(v && v[0] == '0');
			}();
			return enabled;
		}

		float Wrap(float a_angle) { return std::remainder(a_angle, kTwoPi); }

		// Vanilla Skyrim and nothing to ER.
		const char* SuspendReason(RE::PlayerCharacter* a_player)
		{
			auto* state = a_player->AsActorState();
			g.midair = a_player->IsInMidair() ? g.midair + 1 : 0;
			if (a_player->IsDead()) {
				return "dead";
			}
			if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
				return "menu";
			}
			if (state->IsSwimming()) {
				return "swimming";
			}
			if (a_player->IsOnMount()) {
				return "mounted";
			}
			if (state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
				return "furniture";
			}
			if (a_player->IsInKillMove()) {
				return "kill move";
			}
			if (state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) {
				return "knocked down";
			}
			if (state->IsStaggered()) {
				return "staggered";
			}
			if (state->IsSneaking()) {
				return "sneaking";
			}
			static const RE::BSFixedString kJumpState{ "bInJumpState" };
			if (bool jumping = false; a_player->GetGraphVariableBool(kJumpState, jumping) && jumping) {
				return "jump";
			}
			if (g.midair >= kMidairFrames) {
				return "falling";
			}
			return nullptr;
		}

		// Skyrim's own action shows; Sprint still rolls through the stage A path.
		const char* YieldReason(RE::PlayerCharacter* a_player)
		{
			if (movement::Active()) {
				return "stage A dodge";
			}
			auto* state = a_player->AsActorState();
			if (state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) {
				return "attack";
			}
			if (a_player->IsBlocking()) {
				return "block";
			}
			switch (state->GetWeaponState()) {
			case RE::WEAPON_STATE::kWantToDraw:
			case RE::WEAPON_STATE::kDrawing:
			case RE::WEAPON_STATE::kWantToSheathe:
			case RE::WEAPON_STATE::kSheathing:
				return "weapon draw/sheathe";
			default:
				break;
			}
			// weaponState is kDrawn/kSheathed a frame after the key (measured 2026-10-05); the graph's flags last the whole animation.
			static const RE::BSFixedString kEquipping{ "IsEquipping" };
			static const RE::BSFixedString kUnequipping{ "IsUnequipping" };
			if (bool v = false; (a_player->GetGraphVariableBool(kEquipping, v) && v) || (a_player->GetGraphVariableBool(kUnequipping, v) && v)) {
				return "weapon draw/sheathe";
			}
			for (const auto source : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
				const auto* caster = a_player->GetMagicCaster(source);
				// kReady = a spell in hand, idle; anything else but kNone = charging/casting.
				if (caster && caster->state.get() != RE::MagicCaster::State::kNone && caster->state.get() != RE::MagicCaster::State::kReady) {
					return "spell";
				}
			}
			return nullptr;
		}

		void Summary(std::uint64_t a_frame, const char* a_when)
		{
			if (g.frames == 0 || g.erDistM < 0.01f) {
				return;
			}
			const float skyM = g.skyDist / coords::kSkyrimUnitsPerM;
			SKSE::log::info("[loco] {}: frames={} ER {:.2f} m, Skyrim {:.2f} m ({:.0f}%) stalls={} facing error max={:.0f} deg anim={} frame={}", a_when,
				g.frames, g.erDistM, skyM, 100.0f * skyM / g.erDistM, g.stalls, g.maxFacingErr * 180.0f / std::numbers::pi_v<float>, g.anim, a_frame);
			g.frames = g.stalls = 0;
			g.erDistM = g.skyDist = g.maxFacingErr = 0;
		}
	}

	bool Running() { return g.running; }
	bool Suspended() { return g.suspended; }
	float Facing() { return g.facing; }
	float CamYaw() { return g.camYaw; }

	input::Move Stick(const input::Move& a_move)
	{
		input::Move m = a_move;
		const float len = std::hypot(m.x, m.y);
		if (len > 1.0f) {
			m = { m.x / len, m.y / len };
		}
		const auto* controls = RE::PlayerControls::GetSingleton();
		if (controls && !controls->data.running) {
			m = { m.x * kWalkStick, m.y * kWalkStick };
		}
		return m;
	}

	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, const input::Move& a_move, bool a_enabled,
		float a_delta, std::uint64_t a_frame)
	{
		const char* why = nullptr;
		bool suspend = false;
		const int midair = g.midair;
		g.midair = 0;  // counted again below only while the suspend checks run
		if (!EnvEnabled()) {
			why = "SKYRIMXER_LOCO=0";
		} else if (!a_player) {
			why = "no player";
		} else if (!a_enabled) {
			why = "bridge off or link down";
		} else if (!a_state || !(a_state->flags & kInWorld)) {
			why = "ER not in the world or stale";
		} else if (g.midair = midair; const char* r = SuspendReason(a_player)) {
			why = r;
			suspend = true;
		} else if (const char* y = YieldReason(a_player)) {
			why = y;
		}
		g.suspended = suspend;
		const bool run = why == nullptr;
		if (run && !g.running) {
			const auto& s = *a_state;
			g.w = Wrap(a_player->GetAngleZ() - s.yaw);
			g.facing = a_player->GetAngleZ();
			g.target = g.lastHere = a_player->GetPosition();
			g.haveLast = false;
			g.steerFrames = 0;
			g.frames = g.stalls = 0;
			g.erDistM = g.skyDist = g.maxFacingErr = 0;
			movement::Reset();
			SKSE::log::info("[loco] start W={:.3f} (heading {:.3f}, ER yaw {:.3f}, ER anim {}) frame={}", g.w, a_player->GetAngleZ(), s.yaw, s.anim_id, a_frame);
		} else if (!run && g.running) {
			Summary(a_frame, "last");
			hooks::ClearVelocityOverride();
			// A roll ER started under locomotion stays ER's motion only; the stage A path waits for the next dodge.
			movement::Reset(g.anim >= 0 && (g.anim % 1000000) / 1000 == 27);
			SKSE::log::info("[loco] stop ({}{}) frame={}", why, suspend ? ", vanilla, nothing to ER" : ", vanilla + Sprint = ER dodge", a_frame);
		} else if (!run && why != g.why && a_player) {
			SKSE::log::info("[loco] vanilla: {} frame={}", why, a_frame);
		}
		g.running = run;
		g.why = why;
		if (!run) {
			return;
		}

		const auto& s = *a_state;
		// Skyrim's own locomotion stays idle: the keys go to ER (hooks/MoveSwallow refuses them; a key held when the mode began must not
		// keep Skyrim running underneath).
		if (auto* controls = RE::PlayerControls::GetSingleton()) {
			controls->data.moveInputVec = RE::NiPoint2{ 0.0f, 0.0f };
		}
		g.facing = Wrap(s.yaw + g.w);
		g.camYaw = Wrap(movement::LookYaw(a_player) - g.w);
		if (s.anim_id != g.anim) {
			g.anim = s.anim_id;
			SKSE::log::info("[loco] ER anim {} frame={}", s.anim_id, a_frame);
		}
		// ER's displacement this frame (interpolated, so a little every Skyrim frame), turned into Skyrim's world by W.
		RE::NiPoint3 step{};
		float erStepM = 0;
		if (g.haveLast) {
			const coords::Vec3 d{ s.pos[0] - g.lastEr[0], 0.0f, s.pos[2] - g.lastEr[2] };
			const float m = std::hypot(d[0], d[2]);
			if (m <= kMaxStepM) {
				auto local = coords::ErDeltaToLocal(d, s.yaw);
				local.up = 0;
				const auto sky = coords::LocalToSkyrimDelta(local, g.facing);
				step = RE::NiPoint3{ sky[0], sky[1], 0.0f };
				erStepM = m;
			}
		}
		g.lastEr = { s.pos[0], s.pos[1], s.pos[2] };
		g.haveLast = true;
		auto* controller = a_player->GetCharController();
		if (a_delta <= 0 || !controller) {
			hooks::ClearVelocityOverride();
			return;
		}
		const auto here = a_player->GetPosition();
		const auto speed = movement::Follow(g.target, here, step, a_delta);
		const float scale = RE::bhkWorld::GetWorldScale();
		hooks::SetVelocityOverride(controller, speed.x * scale, speed.y * scale);

		// Telemetry: realized distance vs ER's, stalls, and how far the body's facing is from where the player steers.
		const float real = std::hypot(here.x - g.lastHere.x, here.y - g.lastHere.y);
		g.lastHere = here;
		if (erStepM > 0 || g.frames > 0) {
			++g.frames;
			g.erDistM += erStepM;
			g.skyDist += real;
			g.stalls += (real / a_delta < 5.0f && erStepM / a_delta > 0.5f) ? 1 : 0;
		}
		const bool steering = a_move.x != 0 || a_move.y != 0;
		g.steerFrames = steering ? g.steerFrames + 1 : 0;
		if (g.steerFrames > kTurnInFrames) {
			const float steer = movement::LookYaw(a_player) + std::atan2(a_move.x, a_move.y);
			g.maxFacingErr = std::max(g.maxFacingErr, std::fabs(Wrap(g.facing - steer)));
		}
		if (g.frames >= kSummaryFrames) {
			Summary(a_frame, "moving");
		}
	}
}
