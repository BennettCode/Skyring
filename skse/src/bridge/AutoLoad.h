#pragma once

// Dev auto-load: when the environment variable SKYRIMXER_AUTOLOAD is set (tools/launch.ps1 -AutoLoad, used by tools/dev.ps1), the
// first time the main menu opens the plugin loads a save itself, so the agent can restart Skyrim without the user.
// "1" = the most recent save; any other value = that save's file name (without .ess). Unset (players) = nothing happens.
namespace sxer::autoload
{
	// Call on kDataLoaded (the UI exists then; the main menu opens right after).
	void Install();
}
