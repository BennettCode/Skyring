#pragma once

#include <optional>

#include "bridge/Input.h"
#include "skyrimxer_protocol.h"

// The Skyrim player follows ER's dodge movement (P4 step 4). While ER plays a dodge animation (27xxx: rolls, backsteps), ER's
// per-frame position delta is taken into the character's own frame (coords::ErDeltaToLocal with ER's yaw) and laid along the roll
// direction in Skyrim (the camera's yaw plus the move-key angle at the roll start), then handed to Skyrim's character controller as its
// velocity (hooks/ControllerVelocity), so its Havok step (and collision) moves the player. Main thread only.
namespace sxer::movement
{
	// Every frame. a_state = ER's PlayerState interpolated on Skyrim's clock (bridge/Timeline; nullopt when stale/disconnected); a_enabled = bridge on;
	// a_pressed = Dodge was pressed this frame.
	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, const input::Move& a_move, bool a_enabled,
		bool a_pressed, float a_delta, std::uint64_t a_frame);
	// True while a dodge moves the player (Skyrim's own movement keys should do nothing then).
	bool Active();
	// The roll direction (heading convention) while a dodge moves the player: ER's forward is laid along it, so the posed body faces it.
	std::optional<float> RollHeading();
	// True from the moment the player steered out of a dodge (ER's move-cancel window) until that dodge animation ends: Skyrim moves the
	// player again, so the ER pose must not keep playing the recovery.
	bool HandedBack();
	// Forget the current dodge without logging it (bridge/Locomotion takes the player's movement over or hands it back; the next call
	// starts fresh). a_spent = ER is in the middle of a dodge it started under locomotion: this path must not pick it up half-way (nor show
	// its pose over the action that ended locomotion); the next dodge animation starts normally.
	void Reset(bool a_spent = false);
	// Where the player looks: the camera's yaw (heading convention), or the body's heading without a camera.
	float LookYaw(const RE::PlayerCharacter* a_player);
	// The follow step (stage A), shared with bridge/Locomotion: moves a_target by a_step (Skyrim units, this frame's ER step) and returns the
	// velocity (units/s) that steers the player at a_here onto it: the step itself (feed-forward) plus a gentle pull on the gap left from
	// before, capped. The target never runs more than a short lead ahead (a wall stops the player, so it holds the target too).
	// a_lead = the gap before this frame's step (for the logs).
	RE::NiPoint3 Follow(RE::NiPoint3& a_target, const RE::NiPoint3& a_here, const RE::NiPoint3& a_step, float a_delta, float* a_lead = nullptr);
}
