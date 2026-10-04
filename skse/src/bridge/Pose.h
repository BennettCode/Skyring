#pragma once

#include <cstdint>
#include <optional>

#include "skyrimxer_protocol.h"

// Pose streaming, Skyrim side (docs/POSE-PLAN.md step 5): ER's skeleton pose (PoseState) is written onto the player's third-person
// skeleton right after PlayerCharacter::Update (vfunc 0xAD), where step 2 proved a write shows on screen. Main thread only.
namespace sxer::pose
{
	// Every frame from bridge::OnFrame, after movement::Update.
	// a_pose = the fresh PoseState (nullopt when stale, disconnected or the bridge is off: the body blends back to Skyrim's animation).
	// a_facing = heading (GetAngleZ convention) the posed body faces: the roll direction during a dodge, else the body's own heading.
	void Apply(RE::PlayerCharacter* a_player, const std::optional<proto::PoseState>& a_pose, float a_facing, std::uint64_t a_frame);
}
