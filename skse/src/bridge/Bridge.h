#pragma once

// Owns the plugin's Link and the thread that ticks it every LINK_TICK_MS, and the per-frame slot traffic.
namespace sxer::bridge
{
	// Starts the link thread and installs the frame hook + input sink (called on kDataLoaded). Later calls do nothing.
	void Start();
	// Main thread, once per game frame (hooks/PlayerUpdate.cpp): Sprint → InputState, PlayerState → log edges, perf every 5 s,
	// bounded [coords] samples of the player's position/heading while moving (coordinate test).
	void OnFrame(RE::PlayerCharacter* a_player, float a_delta);
	// True while vanilla sprint must stay off: bridge on (F10), link connected and the ER character in the world (fresh PlayerState).
	// Updated by OnFrame; read by hooks/SprintSwallow.cpp (main thread).
	bool SwallowSprint();
	// One-shot (cleared when read): Sprint has just been held long enough to be Skyrim's sprint. The sprint handler only starts on a
	// fresh press, so hooks/SprintSwallow.cpp presents the next held Sprint event as one.
	bool TakeSprintKick();
}
