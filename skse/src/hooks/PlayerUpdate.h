#pragma once

namespace sxer::hooks
{
	// Per-frame hook on PlayerCharacter::Update; calls bridge::OnFrame after the game's own update. Install once (kDataLoaded).
	void InstallPlayerUpdate();
}
