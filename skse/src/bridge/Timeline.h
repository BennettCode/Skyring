#pragma once

#include <cstdint>
#include <optional>

#include "skyrimxer_protocol.h"

// ER's state on Skyrim's clock (docs/LOCO-PLAN.md stage A). The two games' frames aren't phase-locked, so reading ER's latest
// PlayerState/PoseState each Skyrim frame shows some ER frames twice and skips others (judder). Instead every sample is kept with its
// writer's QueryPerformanceCounter stamp (time_us), and each Skyrim frame renders a moment slightly in the past, interpolating between
// the two samples around it: never extrapolating. The delay follows how late samples actually arrive.
// Approach from SkyCraft skse/src/Game.cpp (MIT, see THIRD-PARTY-NOTICES). Main thread only.
namespace sxer::timeline
{
	struct View
	{
		// Interpolated: pos, yaw, cam_yaw (and time_us = the render time). Discrete fields (flags, anim_id, stamina...) are the earlier sample's.
		std::optional<proto::PlayerState> player;
		// Interpolated pose (per-bone slerp, pelvis offset and yaw lerped); flags from the earlier sample.
		std::optional<proto::PoseState> pose;
		bool late = false;  // the render time is past the newest sample: holding it (the player stands still for this frame)
	};

	// Every Skyrim frame, with this frame's fresh reads (nullopt = no new/valid read; the history keeps what it has).
	void Push(const std::optional<proto::PlayerState>& a_player, const std::optional<proto::PoseState>& a_pose, std::uint64_t a_nowUs);
	// The state to show at a_nowUs. Empty when there's no history (link down, ER not stamping).
	View At(std::uint64_t a_nowUs);
	// Forget everything (link lost, bridge off): the next samples start a new history.
	void Reset();

	// For the [perf] line: current render delay and frames rendered late since the last call.
	float DelayMs();
	std::uint32_t TakeLateFrames();
}
