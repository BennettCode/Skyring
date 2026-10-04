#include "hooks/MoveSwallow.h"

#include "bridge/Movement.h"

// MovementHandler::CanProcess = PlayerInputHandler vfunc 0x1 (CommonLib RE/M/MovementHandler.h; the AE 1.7.99 vfunc shift starts at 0x2):
// while an ER dodge moves the player (bridge/Movement.cpp), the movement keys/stick are refused, so held keys don't keep setting
// PlayerControls' move input (Movement.cpp zeroes it) and Skyrim's running doesn't add to the roll. Held keys send an event every frame,
// so movement resumes on the first frame after the dodge. Our BSInputDeviceManager sink still sees the keys (roll direction).
// A vtable write, so no trampoline. Address Library: VTABLE_MovementHandler[0] = AE id 208715, checked with tools/addrlib-check.ps1.
namespace sxer::hooks
{
	namespace
	{
		struct MovementCanProcessHook
		{
			static bool thunk(RE::MovementHandler* a_this, RE::InputEvent* a_event)
			{
				if (movement::Active()) {
					return false;
				}
				return func(a_this, a_event);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallMoveSwallow()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_MovementHandler[0] };
		MovementCanProcessHook::func = vtable.write_vfunc(0x1, MovementCanProcessHook::thunk);
	}
}
