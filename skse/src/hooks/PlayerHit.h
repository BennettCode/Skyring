#pragma once

namespace sxer::hooks
{
	// P4 step 7: melee hits on the player are cancelled while ER's roll i-frames are on. Install once (kDataLoaded); needs the trampoline.
	void InstallPlayerHit();
	// Every frame from bridge::OnFrame: whether the i-frames of the roll the player sees are on (false when stale / bridge off).
	void SetPlayerIFrame(bool a_on);
	// P5: true once the byte check passed, so Skyrim's apply-hit function (AE 38586) can be called for our own hits.
	bool HitPipelineReady();
	// Applies a_hit to a_victim through Skyrim's own apply-hit function (blood, reactions, stagger, crime, kill credit). False = not
	// available on this build (use a fallback).
	bool ApplyHit(RE::Actor* a_victim, RE::HitData& a_hit);
}
