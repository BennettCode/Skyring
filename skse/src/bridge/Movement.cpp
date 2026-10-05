#include "bridge/Movement.h"

#include <cmath>
#include <numbers>
#include <format>
#include <string>

#include "bridge/Coords.h"
#include "bridge/VanillaInput.h"
#include "hooks/ControllerVelocity.h"

// The player is moved by putting ER's velocity into the character controller where Skyrim's own locomotion sets it
// (hooks/ControllerVelocity.cpp), so Skyrim's Havok step moves the player with collision, one velocity per frame.
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
		// LOCO-PLAN stage A: velocity = this frame's ER step (feed-forward) + this share of the remaining gap per second. Closing the whole
		// gap every frame overshot and stuttered (2026-10-04 playtest: 93 → 355 → −90 u/s).
		constexpr float kCorrectionPerS = 8.0f;
		// Measured 2026-10-05 (5 dodges, all directions): 0 stalled frames, 98-100% of ER's distance; the realized speed is ER's
		// interpolated speed one frame later, so its frame-to-frame change is ER's own (18-35% of peak, ER's roll curve).
		// Actor::ApplyCurrent (before) was refused on alternate frames (22-27 stalls, 89-94%); writing the controller's linear velocity or
		// velocityMod after PlayerCharacter::Update doesn't stick (Skyrim sets both again before the Havok step).
		// Roll animation (P4 step 5): the vanilla Silent Roll is sneak + sprint. Attempt 3: Sneak is pressed through the game's own
		// SneakHandler (vanilla::PressSneak, so HUD/stealth follow), sprint starts once the player sneaks, and the move input holds the
		// roll's key direction so the game keeps the sprint going. The vanilla roll only goes where the body faces, so in third person the
		// body is turned to the roll heading for the roll while the camera's free rotation is set to the same angle the other way (camera
		// stays put), then both are put back. After kVanillaRollS (a vanilla tap-roll: tailSprint .. SprintStop ~0.48 s) sprint stops and
		// Sneak is pressed again. First person only animates rolls within kAnimMaxAngle of the facing. Attempt 2 (bits only, zero move
		// input) cut the roll after ~8 frames, half the tries only crouched and the sneak eye stayed.
		constexpr float kAnimMaxAngle = 1.05f;  // ~60 degrees
		// OFF (2026-10-04, attempt 3): Sneak via the handler works (sneak in/out clean, no lingering eye) and the roll plays, but only
		// forward: the third-person camera puts the body back on the camera yaw every frame (SetHeading + free rotation had no effect).
		// Next: stream ER's own animation pose onto the Skyrim skeleton (STATUS handoff).
		constexpr bool kSneakRollTrick = false;
		constexpr float kVanillaRollS = 0.48f;
		// Frames to wait for the player to sneak, and for the graph to take SprintStart (refused for a frame or two with a weapon out).
		constexpr int kSneakWaitFrames = 6;
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
			std::uint64_t startErFrame = 0;
			// The player steered out of the dodge at ER's move-cancel window: Skyrim moves again and the pose blends out (until the anim ends).
			bool handedBack = false;
			// Smoothness of the realized motion (per dodge): stalled frames, biggest frame-to-frame speed change, peak speed (units/s).
			RE::NiPoint3 lastHere;
			RE::NiPoint3 lastDrawn;  // the 3D root's position: what's on screen (GetPosition may update in another phase)
			int drawnStalls = 0;
			float drawnJump = 0, lastDrawnSpeed = 0;
			float lastRealSpeed = 0;
			float peakSpeed = 0;
			float maxSpeedJump = 0;
			// ER's own speed along the same frames (the interpolated target's step / dt): its biggest frame-to-frame change is the floor
			// for ours. Per-frame series (realized / ER, u/s) logged at the dodge end.
			float lastErSpeed = 0, erJump = 0;
			std::string series;
			int stalls = 0;
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
			// Roll animation: 0 = off, 1 = Sneak pressed (waiting for the player to sneak), 2 = rolling (sprint on).
			int trick = 0;
			bool trickEnteredSneak = false;
			int trickWait = 0;
			float trickTime = 0;
			// Move input held during the roll (PlayerControls moveInputVec convention: x right, y forward, camera-relative).
			RE::NiPoint2 trickDir;
			// Body turned for the roll (third person): camera yaw at the turn and the camera's free-rotation flag before it.
			bool turned = false;
			bool savedFreeRotation = false;
			float turnCamYaw = 0;
			int turnLogFrames = 0;
		} g;

		void Notify(RE::PlayerCharacter* a_player, const char* a_event)
		{
			const bool ok = a_player->NotifyAnimationGraph(a_event);
			SKSE::log::info("[move] roll animation: NotifyAnimationGraph('{}') = {}", a_event, ok);
		}

		RE::ThirdPersonState* ThirdPerson()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->IsInThirdPerson()) {
				return nullptr;
			}
			return static_cast<RE::ThirdPersonState*>(camera->currentState.get());
		}

		// Body faces the roll heading; the camera's free rotation takes the difference so the view doesn't move (every trick frame).
		void TurnBody(RE::PlayerCharacter* a_player)
		{
			auto* tps = ThirdPerson();
			if (!tps) {
				return;
			}
			if (!g.turned) {
				g.turned = true;
				g.savedFreeRotation = tps->freeRotationEnabled;
				g.turnCamYaw = LookYaw(a_player);
				g.turnLogFrames = 3;
				SKSE::log::info("[move] roll animation: body {:.3f} → {:.3f}, camera {:.3f} (free rotation was {} x={:.3f})", a_player->GetAngleZ(),
					g.rollHeading, g.turnCamYaw, tps->freeRotationEnabled, tps->freeRotation.x);
			} else if (g.turnLogFrames > 0) {
				--g.turnLogFrames;
				SKSE::log::info("[move] roll animation: body {:.3f} camera {:.3f} (camera at the turn {:.3f})", a_player->GetAngleZ(), LookYaw(a_player),
					g.turnCamYaw);
			}
			a_player->SetHeading(g.rollHeading);
			tps->freeRotationEnabled = true;
			tps->freeRotation.x = std::remainder(g.turnCamYaw - g.rollHeading, 2.0f * std::numbers::pi_v<float>);
		}

		// Body back to where the camera looks, free rotation as it was.
		void RestoreBody(RE::PlayerCharacter* a_player)
		{
			if (!g.turned) {
				return;
			}
			g.turned = false;
			a_player->SetHeading(g.turnCamYaw);
			if (auto* tps = ThirdPerson()) {
				tps->freeRotation.x = 0;
				tps->freeRotationEnabled = g.savedFreeRotation;
			}
			SKSE::log::info("[move] roll animation: body back to {:.3f}, camera {:.3f}", g.turnCamYaw, LookYaw(a_player));
		}

		// a_keepSneak: a chained roll follows at once (pressing Sneak out and in again within a frame doesn't work).
		void EndTrick(RE::PlayerCharacter* a_player, const char* a_why, bool a_keepSneak = false)
		{
			if (g.trick == 0) {
				return;
			}
			auto* state = a_player->AsActorState();
			if (state->actorState1.sprinting) {
				state->actorState1.sprinting = 0;
				Notify(a_player, "SprintStop");
			}
			if (auto* controls = RE::PlayerControls::GetSingleton()) {
				controls->data.moveInputVec = { 0.0f, 0.0f };
			}
			RestoreBody(a_player);
			const bool before = a_player->IsSneaking();
			bool pressed = false;
			if (g.trickEnteredSneak && before && !a_keepSneak) {
				pressed = vanilla::PressSneak();
			}
			SKSE::log::info("[move] roll animation end ({}) after {:.2f} s: sneak {} → {} (Sneak pressed: {})", a_why, g.trickTime, before,
				a_player->IsSneaking(), pressed);
			g.trick = 0;
			if (!a_keepSneak) {
				g.trickEnteredSneak = false;
			}
		}

		bool IsDodgeAnim(std::int32_t a_anim) { return a_anim >= 0 && (a_anim % 1000000) / 1000 == 27; }
		// Backsteps 27010 (+ stance prefix); rolls are 271xx.
		bool IsBackstepAnim(std::int32_t a_anim) { return a_anim >= 0 && (a_anim % 1000000) / 100 == 270; }

		void End(RE::PlayerCharacter* a_player, std::uint64_t a_frame, const char* a_why, bool a_keepSneak = false)
		{
			const auto pos = a_player->GetPosition();
			const float skyM = std::hypot(pos.x - g.startPos.x, pos.y - g.startPos.y) / coords::kSkyrimUnitsPerM;
			SKSE::log::info("[move] dodge end ({}): anim={} frames={} er_frames={} | ER {:.2f} m, Skyrim {:.2f} m ({:.0f}%) frame={}", a_why, g.anim,
				g.frames, g.erFrames, g.erDistM, skyM, g.erDistM > 0.01f ? 100.0f * skyM / g.erDistM : 0.0f, a_frame);
			SKSE::log::info("[move] smooth: frames={} stalls={} peak={:.0f} u/s biggest frame-to-frame change={:.0f} u/s ({:.0f}% of peak) | drawn "
							"(3D root): stalls={} biggest change={:.0f} u/s",
				g.frames, g.stalls, g.peakSpeed, g.maxSpeedJump, g.peakSpeed > 1.0f ? 100.0f * g.maxSpeedJump / g.peakSpeed : 0.0f, g.drawnStalls,
				g.drawnJump);
			SKSE::log::info("[move] smooth: ER's own biggest frame-to-frame change={:.0f} u/s ({:.0f}% of peak) | per frame realized/ER u/s: {}", g.erJump,
				g.peakSpeed > 1.0f ? 100.0f * g.erJump / g.peakSpeed : 0.0f, g.series);
			g.active = false;
			g.spent = true;
			EndTrick(a_player, "dodge end", a_keepSneak);
		}

		void Start(RE::PlayerCharacter* a_player, std::int32_t a_anim, const input::Move& a_move, std::uint64_t a_frame, std::uint64_t a_erFrame,
			const char* a_why)
		{
			g.startErFrame = a_erFrame;
			g.handedBack = false;
			g.lastHere = a_player->GetPosition();
			if (auto* root = a_player->Get3D(false)) {
				g.lastDrawn = root->world.translate;
			}
			g.lastRealSpeed = g.peakSpeed = g.maxSpeedJump = 0;
			g.lastErSpeed = g.erJump = 0;
			g.series.clear();
			g.stalls = 0;
			g.drawnStalls = 0;
			g.drawnJump = g.lastDrawnSpeed = 0;
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
			// Vanilla roll animation: press Sneak now, sprint once the player sneaks (Update).
			const float off = std::remainder(g.rollHeading - a_player->GetAngleZ(), 2.0f * std::numbers::pi_v<float>);
			const auto* camera = RE::PlayerCamera::GetSingleton();
			const bool firstPerson = camera && camera->IsInFirstPerson();
			EndTrick(a_player, "new dodge", true);
			if (kSneakRollTrick && steering && (!firstPerson || std::fabs(off) <= kAnimMaxAngle) && vanilla::HasRollPerk(a_player)) {
				const float len = std::hypot(a_move.x, a_move.y);
				g.trickDir = { a_move.x / len, a_move.y / len };
				g.trickWait = 0;
				g.trickTime = 0;
				const bool sneaking = a_player->IsSneaking();
				const bool pressed = !sneaking && vanilla::PressSneak();
				// A chained roll keeps the sneak the previous roll entered (EndTrick a_keepSneak).
				g.trickEnteredSneak = pressed || (sneaking && g.trickEnteredSneak);
				g.trick = 1;
				SKSE::log::info("[move] roll animation start: dir=({:.2f},{:.2f}) first_person={} sneak {} → {} (Sneak pressed: {})", g.trickDir.x,
					g.trickDir.y, firstPerson, sneaking, a_player->IsSneaking(), pressed);
			} else if (g.trickEnteredSneak) {
				g.trickEnteredSneak = false;
				if (a_player->IsSneaking()) {
					vanilla::PressSneak();
				}
			}
		}
	}

	float LookYaw(const RE::PlayerCharacter* a_player)
	{
		if (const auto* camera = RE::PlayerCamera::GetSingleton()) {
			return camera->GetRuntimeData2().yaw;
		}
		return a_player->GetAngleZ();
	}

	RE::NiPoint3 Follow(RE::NiPoint3& a_target, const RE::NiPoint3& a_here, const RE::NiPoint3& a_step, float a_delta, float* a_lead)
	{
		a_target += a_step;
		RE::NiPoint3 err{ a_target.x - a_here.x, a_target.y - a_here.y, 0.0f };
		const float lead = std::hypot(err.x, err.y);
		if (a_lead) {
			*a_lead = lead;
		}
		if (lead > kMaxLeadUnits) {
			err = err * (kMaxLeadUnits / lead);
			a_target = { a_here.x + err.x, a_here.y + err.y, a_target.z };
		}
		// Feed-forward: this frame's ER step, plus a gentle pull on whatever gap is left from before.
		const RE::NiPoint3 gap{ err.x - a_step.x, err.y - a_step.y, 0.0f };
		auto speed = a_step * (1.0f / a_delta) + gap * kCorrectionPerS;
		const float v = std::hypot(speed.x, speed.y);
		if (v > kMaxSpeed) {
			speed = speed * (kMaxSpeed / v);
		}
		return speed;
	}

	void Reset(bool a_spent)
	{
		g.active = false;
		g.spent = a_spent;
		g.handedBack = a_spent;
		g.haveLast = false;
		g.lastIframe = false;
		g.pressed = false;
	}

	bool Active() { return g.active; }

	std::optional<float> RollHeading() { return g.active ? std::optional<float>(g.rollHeading) : std::nullopt; }

	bool HandedBack() { return g.handedBack; }

	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, const input::Move& a_move, bool a_enabled,
		bool a_pressed, float a_delta, std::uint64_t a_frame)
	{
		vanilla::UpdateRollPerk(a_player, kSneakRollTrick && a_enabled, a_frame);
		if (a_pressed) {
			g.pressFrame = a_frame;
			g.pressed = true;
		}
		if (!a_player || !a_enabled || !a_state) {
			hooks::ClearVelocityOverride();
			if (g.active && a_player) {
				End(a_player, a_frame, a_enabled ? "ER state stale" : "bridge off");
			} else if (a_player) {
				EndTrick(a_player, "bridge off or ER stale");
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
			g.handedBack = false;
		} else if (!g.active && !g.spent) {
			Start(a_player, s.anim_id, a_move, a_frame, s.frame, "new dodge anim");
		} else if (iframeRise && g.pressed && a_frame - g.pressFrame <= kChainPressFrames && (!g.active || g.erFrames >= kMinChainErFrames)) {
			// Rolls chained by spamming keep the same animation id (27110 → 27110); each one opens a new i-frame window.
			if (g.active) {
				End(a_player, a_frame, "chained", true);
			}
			Start(a_player, s.anim_id, a_move, a_frame, s.frame, "chained roll");
		} else if (!g.active && IsBackstepAnim(s.anim_id) && g.pressed && a_frame - g.pressFrame <= kChainPressFrames && g.erSpeedMs > kChainSpeedMs) {
			// Chained backsteps keep the animation id and have no i-frames: a fresh press plus ER moving again starts the next one.
			// (Backsteps only: a roll's recovery still moves ER a little, and a press then is the next roll, which opens i-frames.)
			Start(a_player, s.anim_id, a_move, a_frame, s.frame, "chained (press + ER moving)");
		}

		// a_state is interpolated on Skyrim's clock (bridge/Timeline): its position moves a little every Skyrim frame, so the step is
		// taken every frame (no more "new ER frame" steps, which came in bursts).
		RE::NiPoint3 step;
		bool fresh = false;
		if (g.haveLast) {
			const coords::Vec3 d{ pos[0] - g.lastErPos[0], 0.0f, pos[2] - g.lastErPos[2] };
			const float m = std::hypot(d[0], d[2]);
			g.erSpeedMs = m <= kMaxStepM && a_delta > 0 ? m / a_delta : 0.0f;
			if (g.active && m <= kMaxStepM) {
				auto local = coords::ErDeltaToLocal(d, s.yaw);
				local.up = 0;
				const auto sky = coords::LocalToSkyrimDelta(local, g.rollHeading);
				step = RE::NiPoint3{ sky[0], sky[1], 0.0f };
				g.erDistM += m;
				g.erFrames = static_cast<int>(s.frame - g.startErFrame);
				fresh = m > 0;
				g.slowFrames = (g.erFrames >= kMinErFrames && g.erSpeedMs < kStopSpeedMs) ? g.slowFrames + 1 : 0;
			}
		}
		g.lastErPos = pos;
		g.lastErFrame = s.frame;
		g.haveLast = true;
		const bool steering = a_move.x != 0 || a_move.y != 0;
		if (g.active && (s.flags & kMoveCancel) && g.erFrames >= kMinCancelErFrames && steering) {
			// The player wants to move on: in ER the recovery would cancel into running here.
			End(a_player, a_frame, "ER move-cancel window, player steering");
			g.handedBack = true;
		} else if (g.active && g.slowFrames >= kStopFrames) {
			End(a_player, a_frame, "ER motion over");
		}
		if (!g.active || a_delta <= 0) {
			hooks::ClearVelocityOverride();
			return;
		}
		++g.frames;
		if (g.trick) {
			TurnBody(a_player);
		}
		if (g.trick == 1) {
			bool started = false;
			if (a_player->IsSneaking()) {
				auto& state1 = a_player->AsActorState()->actorState1;
				state1.sprinting = 1;
				started = a_player->NotifyAnimationGraph("SprintStart");
				if (!started) {
					state1.sprinting = 0;
				}
				SKSE::log::info("[move] roll animation: SprintStart = {} (try {})", started, g.trickWait + 1);
			}
			if (started) {
				g.trick = 2;
			} else if (++g.trickWait > kSneakWaitFrames) {
				EndTrick(a_player, a_player->IsSneaking() ? "graph refused SprintStart" : "player never sneaked");
			}
		} else if (g.trick == 2) {
			g.trickTime += a_delta;
			if (!a_player->AsActorState()->actorState1.sprinting) {
				EndTrick(a_player, "game ended the sprint");
			} else if (g.trickTime >= kVanillaRollS) {
				EndTrick(a_player, "roll time over");
			}
		}
		if (auto* controls = RE::PlayerControls::GetSingleton()) {
			// Forward: the body faces the roll (third person) or is within kAnimMaxAngle of it (first person); vanilla sprint only runs forward.
			controls->data.moveInputVec = g.trick ? RE::NiPoint2{ 0.0f, 1.0f } : RE::NiPoint2{ 0.0f, 0.0f };
		}
		// Steer onto the target: the velocity that closes the gap this frame, capped; a blocked player drags the target along.
		const auto here = a_player->GetPosition();
		// Realized motion since last frame (smoothness stats).
		const float real = std::hypot(here.x - g.lastHere.x, here.y - g.lastHere.y) / a_delta;
		if (g.frames > 2) {
			g.stalls += (real < 5.0f && g.erSpeedMs > 0.5f) ? 1 : 0;
			g.maxSpeedJump = std::max(g.maxSpeedJump, std::fabs(real - g.lastRealSpeed));
		}
		g.peakSpeed = std::max(g.peakSpeed, real);
		if (auto* root = a_player->Get3D(false)) {
			const auto& d = root->world.translate;
			const float drawn = std::hypot(d.x - g.lastDrawn.x, d.y - g.lastDrawn.y) / a_delta;
			if (g.frames > 2) {
				g.drawnStalls += (drawn < 5.0f && g.erSpeedMs > 0.5f) ? 1 : 0;
				g.drawnJump = std::max(g.drawnJump, std::fabs(drawn - g.lastDrawnSpeed));
			}
			g.lastDrawnSpeed = drawn;
			g.lastDrawn = d;
		}
		const float erSpeed = std::hypot(step.x, step.y) / a_delta;
		if (g.frames > 2) {
			g.erJump = std::max(g.erJump, std::fabs(erSpeed - g.lastErSpeed));
		}
		g.lastErSpeed = erSpeed;
		g.series += std::format("{:.0f}/{:.0f} ", real, erSpeed);
		g.lastRealSpeed = real;
		g.lastHere = here;
		float lead = 0;
		const auto speed = Follow(g.target, here, step, a_delta, &lead);
		const float scale = RE::bhkWorld::GetWorldScale();
		if (auto* controller = a_player->GetCharController()) {
			hooks::SetVelocityOverride(controller, speed.x * scale, speed.y * scale);
		}
		if (g.frames <= kDetailFrames) {
			SKSE::log::info("[move] frame+{}: step=({:.1f},{:.1f}) units{} lead={:.1f} speed=({:.0f},{:.0f}) u/s dt={:.4f} er_frame={}", g.frames,
				step.x, step.y, fresh ? "" : " (no new ER frame)", lead, speed.x, speed.y, a_delta, s.frame);
		}
	}
}
