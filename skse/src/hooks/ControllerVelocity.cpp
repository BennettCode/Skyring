#include "hooks/ControllerVelocity.h"

#include <atomic>
#include <bit>
#include <cstdint>

// bhkCharProxyController::SetLinearVelocityImpl = bhkCharacterController vfunc 0x07 (CommonLib RE/B/bhkCharacterController.h): where
// Skyrim's own locomotion hands the character controller its velocity before the Havok step integrates it with collision. Called once
// per frame per character from a Havok worker thread (measured 2026-10-05), so the override is read lock-free.
// Measured 2026-10-05: writing the linear velocity or velocityMod after PlayerCharacter::Update doesn't stick (Skyrim sets both again
// before the step), and Actor::ApplyCurrent is refused on alternate frames; overriding here gives 0 stalled frames.
// Address Library: VTABLE_bhkCharProxyController[1] = AE id 240560, checked with tools/addrlib-check.ps1. [1], not [0]: the class puts
// hkpCharacterProxyListener first, so its bhkCharacterController part (what GetCharController returns) uses the second vtable. The
// player's controller is a bhkCharProxyController (its vtable = that id, measured 2026-10-05). A vtable write, no trampoline.
namespace sxer::hooks
{
	namespace
	{
		std::atomic<const RE::bhkCharacterController*> g_controller{ nullptr };
		// x and y as one 64-bit value so the worker thread never reads a torn pair.
		std::atomic<std::uint64_t> g_velocity{ 0 };

		struct SetLinearVelocityHook
		{
			static void thunk(RE::bhkCharacterController* a_this, const RE::hkVector4& a_velocity)
			{
				if (a_this != g_controller.load(std::memory_order_acquire)) {
					func(a_this, a_velocity);
					return;
				}
				const auto packed = g_velocity.load(std::memory_order_relaxed);
				float v[4];
				_mm_storeu_ps(v, a_velocity.quad);
				func(a_this, RE::hkVector4(std::bit_cast<float>(static_cast<std::uint32_t>(packed)),
								 std::bit_cast<float>(static_cast<std::uint32_t>(packed >> 32)), v[2], v[3]));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void SetVelocityOverride(const RE::bhkCharacterController* a_controller, float a_x, float a_y)
	{
		g_velocity.store(std::bit_cast<std::uint32_t>(a_x) | (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(a_y)) << 32),
			std::memory_order_relaxed);
		g_controller.store(a_controller, std::memory_order_release);
	}

	void ClearVelocityOverride() { g_controller.store(nullptr, std::memory_order_release); }

	void InstallControllerVelocity()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_bhkCharProxyController[1] };
		SetLinearVelocityHook::func = vtable.write_vfunc(0x07, SetLinearVelocityHook::thunk);
	}
}
