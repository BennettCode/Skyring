#pragma once

#include <cstdint>
#include <optional>

#include "skyrimxer_protocol.h"

// P4 step 6: Skyrim's stamina bar shows ER's stamina (ER owns stamina, DESIGN §3). Every frame with a fresh, in-world PlayerState the
// player's Stamina is set to the same share of its maximum as ER's, through the damage modifier. Stale or bridge off: Skyrim's own
// stamina is left alone (it regenerates as usual).
namespace sxer::stamina
{
	// Every frame from bridge::OnFrame. a_state = ER's PlayerState on Skyrim's clock (nullopt = stale / bridge off).
	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, std::uint64_t a_frame);
}
