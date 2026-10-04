#pragma once

// Owns the plugin's Link and the thread that ticks it every LINK_TICK_MS, and the per-frame slot traffic.
namespace sxer::bridge
{
	// Starts the link thread and installs the frame hook + input sink (called on kDataLoaded). Later calls do nothing.
	void Start();
	// Main thread, once per game frame (hooks/PlayerUpdate.cpp): Sprint → InputState, PlayerState → log edges, perf every 5 s,
	// bounded [coords] samples of the player's position/heading while moving (coordinate test).
	void OnFrame(const RE::PlayerCharacter* a_player, float a_delta);
}
