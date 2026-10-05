#pragma once

#include "bridge/CombatMath.h"
#include "skyrimxer_protocol.h"

#include <cstdint>
#include <optional>

// P5 step 1: ER's swings land on Skyrim NPCs. ER says *when* (PlayerState AttackActive / attack_seq) and *how much* (atk_*: attack
// rating x motion value per damage type, atk_poise); Skyrim picks *who* (actors in reach, in front of the body the player sees) and
// applies the hit through its own hit pipeline (blood, sound, reactions, crime, kill credit). docs/P5-PLAN.md.
namespace sxer::combat
{
	// Reads SkyrimXER.ini [Combat] once (kDataLoaded): fDamageScale, the one tuning number (default 1).
	void LoadConfig();
	// Every frame from bridge::OnFrame (main thread). a_state = the freshest PlayerState (nullopt = bridge off / stale: nothing lands);
	// a_facing = the heading of the body the player sees (Locomotion/Movement), radians.
	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, float a_facing, std::uint64_t a_frame);

}
