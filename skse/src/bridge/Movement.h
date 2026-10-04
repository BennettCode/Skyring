#pragma once

#include <optional>

#include "bridge/Input.h"
#include "skyrimxer_protocol.h"

// The Skyrim player follows ER's dodge movement (P4 step 4). While ER plays a dodge animation (27xxx: rolls, backsteps), ER's
// per-frame position delta is taken into the character's own frame (coords::ErDeltaToLocal with ER's yaw) and laid along the roll
// direction in Skyrim (the camera's yaw plus the move-key angle at the roll start), then pushed through Actor::ApplyCurrent so
// Skyrim's character controller (and its collision) moves the player. Main thread only.
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
}
