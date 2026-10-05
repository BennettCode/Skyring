#pragma once

namespace sxer::hooks
{
	// Vanilla attack/block from mouse/keyboard off while the bridge sends them to ER (bridge::SwallowAttack()). Install once (kDataLoaded).
	void InstallAttackSwallow();
}
