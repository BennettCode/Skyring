#include "bridge/Movement.h"

#include <cmath>
#include <numbers>

#include "bridge/Coords.h"

// Actor::ApplyCurrent = Actor vfunc 0x9D (CommonLib RE/A/Actor.h; called through the player's own vtable, no Address Library id):
// Skyrim's "current" push (water currents), a velocity the character controller applies for a time, with collision.
// PlayerCamera singleton = AE id 400802 (CommonLib), checked with tools/addrlib-check.ps1: rolls go where the camera looks (in third
// person the body lags behind a fast-turning camera). PlayerControls singleton = AE id 400864 (CommonLib), checked with tools/addrlib-check.ps1: its move input is zeroed during a dodge
// (hooks/MoveSwallow.cpp keeps the movement keys from setting it again), so Skyrim's own running doesn't add to the roll.
// The player's facing is left alone: turning it turns the third-person camera with it (a 90-180 degree flip on side/back rolls).
namespace sxer::movement
{
	namespace
	{
		// Larger ER jumps between two frames than this are teleports/reloads, never dodge movement.
		constexpr float kMaxStepM = 1.5f;
		// Detail lines (per-frame) logged for the first frames of each dodge.
		constexpr int kDetailFrames = 4;
		// ER's frame rate (measured 59.9-60.0 hidden): ER steps per ER frame become a speed.
		constexpr float kErFps = 60.0f;
		// The dodge hands movement back once ER's character slows below kStopSpeedMs for kStopFrames ER frames, after at least
		// kMinErFrames (the roll's own wind-up is slow too). ER's 1.65 s roll animation is mostly standing recovery after ~0.6 s.
		constexpr int kMinErFrames = 20;
		constexpr float kStopSpeedMs = 0.5f;
		constexpr int kStopFrames = 2;
		// A new i-frame window counts as a new (chained) roll only this many ER frames into the current one.
		constexpr int kMinChainErFrames = 8;
		constexpr std::uint32_t kIFrame = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::IFrame);
		// ER's own "movement may cancel the animation now" window (TAE CANCEL_LS_MOVEMENT): the dodge hands movement back then.
		constexpr std::uint32_t kMoveCancel = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::MoveCancel);
		// Ignore a cancel window this early in a dodge (a previous animation's window can still be open on its first frames).
		constexpr int kMinCancelErFrames = 6;
		// A dodge press followed by ER moving faster than kChainSpeedMs within kChainPressFrames = a new dodge in the same animation
		// (chained backsteps: same id, no i-frames).
		constexpr std::uint64_t kChainPressFrames = 30;
		// Closed loop: the player is steered onto a target that moves with ER. The target never runs further ahead than kMaxLeadUnits
		// (a wall stops the player, so the target stops too), and the push is capped at kMaxSpeed.
		constexpr float kMaxLeadUnits = 60.0f;
		constexpr float kMaxSpeed = 1500.0f;
		// The vanilla sneak-sprint roll (Silent Roll perk) is played for rolls within this angle of the body's facing (it only rolls
		// forward); other directions slide. Experiment (P4 step 5, attempt 2): the vanilla roll is the sneak sprint (graph notifies
		// tailSprint .. SprintStop), so the player is put into sneak (SneakStart) and sprints (SprintStart) one frame later; both are
		// undone when the dodge ends.
		constexpr float kAnimMaxAngle = 1.05f;  // ~60 degrees
		// OFF (2026-10-04, attempt 2 of 2): the roll starts (tailSprint + StartAnimatedCameraDelta) but SprintStop follows ~8 frames
		// later (vanilla: ~29), about half the tries only crouch, and the HUD sneak eye stays up afterwards. See docs/P4-PLAN.md step 5.
		constexpr bool kSneakRollTrick = false;
		constexpr float kChainSpeedMs = 1.0f;

		struct State
		{
			bool active = false;
			// The current dodge animation already handed movement back (only a new roll restarts it).
			bool spent = false;
			bool lastIframe = false;
			bool haveLast = false;
			std::uint64_t lastErFrame = 0;
			std::array<float, 3> lastErPos{};
			// Current dodge.
			float rollHeading = 0;
			std::int32_t anim = 0;
			RE::NiPoint3 startPos;
			float erDistM = 0;
			// Where ER's movement says the player should be (Skyrim world XY).
			RE::NiPoint3 target;
			int frames = 0;
			int erFrames = 0;
			int slowFrames = 0;
			// ER's horizontal speed at the last ER frame (m/s), and the Skyrim frame of the last Dodge press.
			float erSpeedMs = 0;
			std::uint64_t pressFrame = 0;
			bool pressed = false;
			// Sneak-roll trick state: 0 = off, 1 = sneak started (sprint next frame), 2 = sprinting.
			int trick = 0;
			bool trickWasSneaking = false;
		} g;

		void Notify(RE::PlayerCharacter* a_player, const char* a_event)
		{
			const bool ok = a_player->NotifyAnimationGraph(a_event);
			SKSE::log::info("[move] roll animation: NotifyAnimationGraph('{}') = {}", a_event, ok);
		}

		void EndTrick(RE::PlayerCharacter* a_player)
		{
			if (g.trick == 0) {
				return;
			}
			auto* state = a_player->AsActorState();
			if (g.trick == 2) {
				state->actorState1.sprinting = 0;
				Notify(a_player, "SprintStop");
			}
			if (!g.trickWasSneaking) {
				state->actorState1.sneaking = 0;
				Notify(a_player, "SneakStop");
			}
			g.trick = 0;
		}

		// Where the player looks: the camera's yaw (heading convention), or the body's heading without a camera.
		float LookYaw(const RE::PlayerCharacter* a_player)
		{
			if (const auto* camera = RE::PlayerCamera::GetSingleton()) {
				return camera->GetRuntimeData2().yaw;
			}
			return a_player->GetAngleZ();
		}

		bool IsDodgeAnim(std::int32_t a_anim) { return a_anim >= 0 && (a_anim % 1000000) / 1000 == 27; }

		void End(RE::PlayerCharacter* a_player, std::uint64_t a_frame, const char* a_why)
		{
			const auto pos = a_player->GetPosition();
			const float skyM = std::hypot(pos.x - g.startPos.x, pos.y - g.startPos.y) / coords::kSkyrimUnitsPerM;
			SKSE::log::info("[move] dodge end ({}): anim={} frames={} er_frames={} | ER {:.2f} m, Skyrim {:.2f} m ({:.0f}%) frame={}", a_why, g.anim,
				g.frames, g.erFrames, g.erDistM, skyM, g.erDistM > 0.01f ? 100.0f * skyM / g.erDistM : 0.0f, a_frame);
			g.active = false;
			g.spent = true;
			EndTrick(a_player);
		}

		void Start(RE::PlayerCharacter* a_player, std::int32_t a_anim, const input::Move& a_move, std::uint64_t a_frame, const char* a_why)
		{
			// Roll direction: where the player is steering (camera yaw + move-key angle), like an ER roll without lock-on.
			// With no key held (backstep) it's the body's heading: ER's backwards motion maps onto it.
			const bool steering = a_move.x != 0 || a_move.y != 0;
			g.rollHeading = steering ? LookYaw(a_player) + std::atan2(a_move.x, a_move.y) : a_player->GetAngleZ();
			g.active = true;
			g.spent = false;
			g.anim = a_anim;
			g.startPos = a_player->GetPosition();
			g.erDistM = 0;
			g.frames = 0;
			g.erFrames = 0;
			g.slowFrames = 0;
			g.target = g.startPos;
			SKSE::log::info("[move] dodge start ({}): anim={} move={},{} body {:.3f} camera {:.3f} → roll heading {:.3f} frame={}", a_why, a_anim, a_move.x,
				a_move.y, a_player->GetAngleZ(), LookYaw(a_player), g.rollHeading, a_frame);
			g.pressed = false;
			// Vanilla roll animation for forward-ish rolls: enter sneak now, sprint next frame (Update).
			const float off = std::remainder(g.rollHeading - a_player->GetAngleZ(), 2.0f * std::numbers::pi_v<float>);
			EndTrick(a_player);
			if (kSneakRollTrick && steering && std::fabs(off) <= kAnimMaxAngle) {
				auto* state = a_player->AsActorState();
				g.trickWasSneaking = state->IsSneaking();
				if (!g.trickWasSneaking) {
					state->actorState1.sneaking = 1;
					Notify(a_player, "SneakStart");
				}
				g.trick = 1;
			}
		}
	}

	bool Active() { return g.active; }

	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, const input::Move& a_move, bool a_enabled,
		bool a_pressed, float a_delta, std::uint64_t a_frame)
	{
		if (a_pressed) {
			g.pressFrame = a_frame;
			g.pressed = true;
		}
		if (!a_player || !a_enabled || !a_state) {
			if (g.active && a_player) {
				End(a_player, a_frame, a_enabled ? "ER state stale" : "bridge off");
			} else if (a_player) {
				EndTrick(a_player);
			}
			g.active = false;
			g.spent = false;
			g.haveLast = false;
			g.lastIframe = false;
			return;
		}
		const auto& s = *a_state;
		const std::array<float, 3> pos{ s.pos[0], s.pos[1], s.pos[2] };
		const bool dodging = IsDodgeAnim(s.anim_id);
		const bool iframe = (s.flags & kIFrame) != 0;
		const bool iframeRise = iframe && !g.lastIframe;
		g.lastIframe = iframe;

		if (!dodging) {
			if (g.active) {
				End(a_player, a_frame, "anim over");
			}
			g.spent = false;
		} else if (!g.active && !g.spent) {
			Start(a_player, s.anim_id, a_move, a_frame, "new dodge anim");
		} else if (iframeRise && g.pressed && a_frame - g.pressFrame <= kChainPressFrames && (!g.active || g.erFrames >= kMinChainErFrames)) {
			// Rolls chained by spamming keep the same animation id (27110 → 27110); each one opens a new i-frame window.
			if (g.active) {
				End(a_player, a_frame, "chained");
			}
			Start(a_player, s.anim_id, a_move, a_frame, "chained roll");
		} else if (!g.active && g.pressed && a_frame - g.pressFrame <= kChainPressFrames && g.erSpeedMs > kChainSpeedMs) {
			// Chained backsteps keep the animation id and have no i-frames: a fresh press plus ER moving again starts the next one.
			Start(a_player, s.anim_id, a_move, a_frame, "chained (press + ER moving)");
		}

		RE::NiPoint3 step;
		bool fresh = false;
		if (g.haveLast && s.frame != g.lastErFrame) {
			const coords::Vec3 d{ pos[0] - g.lastErPos[0], 0.0f, pos[2] - g.lastErPos[2] };
			const float m = std::hypot(d[0], d[2]);
			g.erSpeedMs = m <= kMaxStepM ? m * kErFps / static_cast<float>(s.frame - g.lastErFrame) : 0.0f;
			if (g.active && m <= kMaxStepM) {
				auto local = coords::ErDeltaToLocal(d, s.yaw);
				local.up = 0;
				const auto sky = coords::LocalToSkyrimDelta(local, g.rollHeading);
				step = RE::NiPoint3{ sky[0], sky[1], 0.0f };
				const auto n = static_cast<float>(s.frame - g.lastErFrame);
				g.target += step;
				g.erDistM += m;
				g.erFrames += static_cast<int>(n);
				fresh = true;
				const float speedMs = m * kErFps / n;
				g.slowFrames = (g.erFrames >= kMinErFrames && speedMs < kStopSpeedMs) ? g.slowFrames + 1 : 0;
			}
		}
		if (s.frame != g.lastErFrame || !g.haveLast) {
			g.lastErPos = pos;
			g.lastErFrame = s.frame;
			g.haveLast = true;
		}
		if (g.active && (s.flags & kMoveCancel) && g.erFrames >= kMinCancelErFrames) {
			End(a_player, a_frame, "ER move-cancel window");
		} else if (g.active && g.slowFrames >= kStopFrames) {
			End(a_player, a_frame, "ER motion over");
		}
		if (!g.active || a_delta <= 0) {
			return;
		}
		++g.frames;
		if (g.trick == 1) {
			a_player->AsActorState()->actorState1.sprinting = 1;
			Notify(a_player, "SprintStart");
			g.trick = 2;
		}
		if (auto* controls = RE::PlayerControls::GetSingleton()) {
			controls->data.moveInputVec = { 0.0f, 0.0f };
		}
		// Steer onto the target: the velocity that closes the gap this frame, capped; a blocked player drags the target along.
		const auto here = a_player->GetPosition();
		RE::NiPoint3 err{ g.target.x - here.x, g.target.y - here.y, 0.0f };
		const float lead = std::hypot(err.x, err.y);
		if (lead > kMaxLeadUnits) {
			err = err * (kMaxLeadUnits / lead);
			g.target = { here.x + err.x, here.y + err.y, g.target.z };
		}
		auto speed = err * (1.0f / a_delta);
		const float v = std::hypot(speed.x, speed.y);
		if (v > kMaxSpeed) {
			speed = speed * (kMaxSpeed / v);
		}
		const float scale = RE::bhkWorld::GetWorldScale();
		const RE::hkVector4 velocity(speed.x * scale, speed.y * scale, 0.0f, 0.0f);
		const bool applied = a_player->ApplyCurrent(a_delta, velocity);
		if (g.frames <= kDetailFrames) {
			SKSE::log::info("[move] frame+{}: step=({:.1f},{:.1f}) units{} lead={:.1f} speed=({:.0f},{:.0f}) u/s dt={:.4f} applied={} er_frame={}", g.frames,
				step.x, step.y, fresh ? "" : " (no new ER frame)", lead, speed.x, speed.y, a_delta, applied, s.frame);
		}
	}
}
