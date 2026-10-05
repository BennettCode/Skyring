#pragma once

#include <optional>

#include "bridge/Input.h"
#include "skyrimxer_protocol.h"

// ER drives the player's locomotion (docs/LOCO-PLAN.md stage B). While the bridge is on and the player is in a plain state, ER walks,
// runs, sprints and rolls; Skyrim follows its displacement through the character controller (hooks/ControllerVelocity, Skyrim's collision)
// and shows ER's pose. Skyrim's own movement keys go to ER (hooks/MoveSwallow keeps them from moving Skyrim).
// Mapping: a world offset W = Skyrim heading − ER yaw, fixed each time the mode starts (so turning either camera never turns the body).
// Skyrim facing = ER yaw + W; ER displacement turns by W; Skyrim's look goes to ER as a yaw in ER's world (look − W) and ER turns the keys
// by it relative to its own camera (er-plugin remote.rs).
// Not running = Skyrim plays vanilla, with two levels:
// - suspended (swimming, mounted, furniture, jumping/falling, sneaking, staggered/knocked down, dead, kill move): nothing goes to ER;
// - yield (Skyrim's own attack, block, spell, weapon draw/sheathe, or a stage A dodge still moving the player): the stage A dodge-only
//   path (bridge/Movement) handles Sprint = ER roll.
// SKYRIMXER_LOCO=0 (environment) turns the mode off: stage A behaviour everywhere. Main thread only.
namespace sxer::locomotion
{
	// Every frame from bridge::OnFrame, after the timeline. a_state = ER's PlayerState interpolated on Skyrim's clock (nullopt = stale);
	// a_enabled = bridge on and the link connected.
	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, const input::Move& a_move, bool a_enabled,
		float a_delta, std::uint64_t a_frame);
	// ER drives the player right now.
	bool Running();
	// Not running and nothing may go to ER (Dodge included).
	bool Suspended();
	// While running: the heading (GetAngleZ convention) the posed body faces = ER yaw + W.
	float Facing();
	// While running: InputState.cam_yaw (Skyrim's look as a yaw in ER's world).
	float CamYaw();
	// While running: the move keys for ER (walk toggle = a walk-sized stick).
	input::Move Stick(const input::Move& a_move);
}
