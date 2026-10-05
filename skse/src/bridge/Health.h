#pragma once

#include "skyrimxer_protocol.h"

#include <cstdint>
#include <optional>

// P5 step 3 (protocol v11): ER's HP is the player's health. Every Skyrim health change of the player (hits of any kind, falls, poison,
// potions, regen) is refunded and sent to ER as a share of Skyrim max health (InputState.hurt_total); Skyrim's bar shows ER's HP share.
// While the bridge is on the player is essential, and ER's Downed flag kills it. Adapted from SkyCraft skse/src/Combat.cpp
// (BridgePlayerDamage, HitSink, KillPlayer; MIT).
namespace sxer::health
{
	// TESHitEvent sink for the log's attribution (kDataLoaded).
	void Install();
	// A save was loaded / a new game started: ER refills its HP (respawn_seq) and the next frame primes instead of forwarding.
	void OnLoad();
	// Every frame from bridge::OnFrame, before InputState is written. a_state = the freshest PlayerState (nullopt = bridge off /
	// stale / ER not in the world: Skyrim's own health, player not essential).
	void Update(RE::PlayerCharacter* a_player, const std::optional<proto::PlayerState>& a_state, std::uint64_t a_frame);
	double HurtTotal();
	std::uint32_t RespawnSeq();
}
