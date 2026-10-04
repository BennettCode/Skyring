#include "bridge/Hud.h"

// RE::SendHUDMessage::ShowHUDMessage = the game's debug notification (top-left text). Address Library: AE id 52933, checked with
// tools/addrlib-check.ps1. Must run on the main thread (we call it from PlayerCharacter::Update).
namespace sxer::hud
{
	void Notify(const std::string& a_text)
	{
		SKSE::log::info("[hud] {}", a_text);
		RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false);
	}
}
