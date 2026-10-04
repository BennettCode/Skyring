#pragma once

#include <string>

// Skyrim's top-left HUD notifications, for playtest feedback (what ER did with a Skyrim press). Main thread only.
namespace sxer::hud
{
	// Shows a_text in Skyrim's HUD and logs it as "[hud] ...".
	void Notify(const std::string& a_text);
}
