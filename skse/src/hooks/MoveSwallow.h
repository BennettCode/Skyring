#pragma once

namespace sxer::hooks
{
	// Skyrim's movement keys do nothing while an ER dodge moves the player (movement::Active()). Install once (kDataLoaded).
	void InstallMoveSwallow();
}
