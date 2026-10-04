#pragma once

namespace sxer::hooks
{
	// While set, the player's character controller gets this horizontal velocity (Havok units/s, world x/y) wherever Skyrim's own
	// locomotion sets its linear velocity; Skyrim keeps the vertical part. Set every frame from movement (main thread).
	void SetVelocityOverride(const RE::bhkCharacterController* a_controller, float a_x, float a_y);
	void ClearVelocityOverride();
	// bhkCharProxyController::SetLinearVelocityImpl hook (vfunc 0x07). Install once (kDataLoaded).
	void InstallControllerVelocity();
}
