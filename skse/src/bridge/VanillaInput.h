#pragma once

// The game's own input paths for the vanilla roll animation (P4 step 5, attempt 3). Main thread only.
namespace sxer::vanilla
{
	// Presses Sneak once through PlayerControls' SneakHandler, exactly like a key press (toggles sneak, HUD and stealth state follow).
	// Returns true if the handler took the event.
	bool PressSneak();

	// Silent Roll (00105F23) makes sneak + sprint a roll. a_on = bridge on with a player loaded: the perk is added when missing
	// (rechecked about once a second, a load can drop it) and removed again when the bridge goes off, if this plugin added it.
	void UpdateRollPerk(RE::PlayerCharacter* a_player, bool a_on, std::uint64_t a_frame);

	bool HasRollPerk(RE::PlayerCharacter* a_player);
}
