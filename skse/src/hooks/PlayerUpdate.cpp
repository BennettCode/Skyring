#include "hooks/PlayerUpdate.h"

#include <atomic>
#include <exception>

#include "bridge/Bridge.h"
#include "bridge/Pose.h"

// PlayerCharacter::Update = Actor vfunc 0xAD (CommonLib RE/A/Actor.h): once per frame on the main thread while the game runs
// (menus that pause the game stop it, which makes our InputState go stale on purpose). A vtable write, so no trampoline.
// Address Library: VTABLE_PlayerCharacter[0] = AE id 208040, checked with tools/addrlib-check.ps1.
// Pattern from SkyCraft (skse/src/Game.cpp, PlayerUpdateHook), developed on the same runtime (1.7.104).
namespace sxer::hooks
{
	namespace
	{
		struct PlayerUpdateHook
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				pose::BeforePlayerUpdate(a_this);
				func(a_this, a_delta);
				try {
					pose::AfterPlayerUpdate(a_this);
					bridge::OnFrame(a_this, a_delta);
				} catch (const std::exception& e) {
					static std::atomic<bool> logged{ false };
					if (!logged.exchange(true)) {
						SKSE::log::error("[core] per-frame update threw: {} (logged once)", e.what());
					}
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallPlayerUpdate()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdateHook::func = vtable.write_vfunc(0xAD, PlayerUpdateHook::thunk);
	}
}
