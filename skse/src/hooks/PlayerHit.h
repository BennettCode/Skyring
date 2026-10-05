#pragma once

namespace sxer::hooks
{
	// P4 step 7: melee hits on the player are cancelled while ER's roll i-frames are on. Install once (kDataLoaded); needs the trampoline.
	void InstallPlayerHit();
	// Every frame from bridge::OnFrame: whether the i-frames of the roll the player sees are on (false when stale / bridge off).
	void SetPlayerIFrame(bool a_on);
}
