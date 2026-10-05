#include "hooks/AttackSwallow.h"

#include "bridge/Bridge.h"

// AttackBlockHandler::CanProcess = PlayerInputHandler vfunc 0x1 (same slot as hooks/SprintSwallow.cpp). While the bridge is on and ER is
// in the world, Right/Left Attack/Block presses are ER's Attack/Guard (bridge/Input.cpp forwards them), so the vanilla handler never
// sees them: no Skyrim swing, block, spell cast or bow draw. Releases always pass, so a vanilla attack begun before the bridge took
// over still ends. The pad's R1/R2/L1/L2 never reach Skyrim at all (hooks/XInput.cpp). A vtable write, so no trampoline.
// Address Library: VTABLE_AttackBlockHandler[0] = AE id 208719, checked with tools/addrlib-check.ps1.
namespace sxer::hooks
{
	namespace
	{
		struct AttackCanProcessHook
		{
			static bool thunk(RE::AttackBlockHandler* a_this, RE::InputEvent* a_event)
			{
				if (a_event && a_event->GetEventType() == RE::INPUT_EVENT_TYPE::kButton && bridge::SwallowAttack() &&
					a_event->AsButtonEvent()->IsPressed()) {
					return false;
				}
				return func(a_this, a_event);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallAttackSwallow()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_AttackBlockHandler[0] };
		AttackCanProcessHook::func = vtable.write_vfunc(0x1, AttackCanProcessHook::thunk);
	}
}
