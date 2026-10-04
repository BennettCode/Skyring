#include "hooks/SprintSwallow.h"

#include "bridge/Bridge.h"

// SprintHandler::CanProcess = PlayerInputHandler vfunc 0x1 (CommonLib RE/S/SprintHandler.h; the AE 1.7.99 vfunc shift starts at 0x2):
// PlayerControls asks each handler whether it takes an input event, on the main thread. Returning false for Sprint presses while the
// bridge is on keeps vanilla sprint off, so Sprint is only the ER dodge (our BSInputDeviceManager sink still sees it). Releases always
// pass, so a sprint that began before the bridge took over still ends. A vtable write, so no trampoline.
// Address Library: VTABLE_SprintHandler[0] = AE id 208717, checked with tools/addrlib-check.ps1.
namespace sxer::hooks
{
	namespace
	{
		struct SprintCanProcessHook
		{
			static bool thunk(RE::SprintHandler* a_this, RE::InputEvent* a_event)
			{
				if (a_event && a_event->GetEventType() == RE::INPUT_EVENT_TYPE::kButton && bridge::SwallowSprint()) {
					if (a_event->AsButtonEvent()->IsPressed()) {
						return false;
					}
				}
				return func(a_this, a_event);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallSprintSwallow()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_SprintHandler[0] };
		SprintCanProcessHook::func = vtable.write_vfunc(0x1, SprintCanProcessHook::thunk);
	}
}
