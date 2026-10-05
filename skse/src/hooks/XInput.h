#pragma once

#include <cstdint>

namespace sxer::hooks
{
	// P4 step 8: Skyrim's XInputGetState import answers with the DualSense (bridge/Gamepad) when no real XInput pad does, and
	// maps the buttons to the Elden Ring layout in gameplay while the bridge is on. Install at plugin load (the main menu gets the pad too).
	void InstallXInput();
	// Watches menus open/close for the gameplay layout. Install once on kDataLoaded (needs the UI singleton).
	void InstallXInputMenuWatch();
	// ER buttons the pad holds right now (InputState.buttons bits; 0 in menus, with the bridge off or with no pad).
	std::uint32_t PadErButtons();
	// One-shot (cleared when read): the touchpad was pressed in gameplay, so open Skyrim's map (main thread, bridge::OnFrame).
	bool TakeMapRequest();
}
