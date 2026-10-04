#pragma once

// Research probe (P4 step 5): logs the player's animation graph events ([anim] lines, footsteps skipped, bounded per session), to find
// the events the vanilla sneak roll uses. Main thread only.
namespace sxer::animprobe
{
	// Every frame; (re)registers the sink about once a second (the graph is rebuilt on load / 3D reset).
	void Update(RE::PlayerCharacter* a_player, std::uint64_t a_frame);
}
