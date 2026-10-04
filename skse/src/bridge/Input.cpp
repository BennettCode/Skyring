#include "bridge/Input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <vector>

// Input sink pattern from SkyCraft (skse/src/Input.cpp, InputSink). Address Library: BSInputDeviceManager singleton (AE 402776) and
// UserEvents singleton (AE 402638), both checked with tools/addrlib-check.ps1.
namespace sxer::input
{
	namespace
	{
		// DirectInput scan code of the bridge toggle key.
		constexpr std::uint32_t kToggleKey = 0x44;  // F10
		std::atomic<bool> g_bridgeOn{ true };
		std::atomic<bool> g_sprint{ false };
		std::atomic<int> g_sprintDevice{ -1 };
		std::array<std::atomic<bool>, 8> g_seenDevice{};
		// Held movement keys: forward, back, strafe left, strafe right.
		std::array<std::atomic<bool>, 4> g_move{};
		// Keyboard keys held right now, by DirectInput scan code (debug keys, e.g. F7/F8 for the pose proof).
		std::array<std::atomic<bool>, 256> g_key{};
		// User events already logged once (main thread only), so the log shows which key/button maps to which event.
		std::vector<std::string> g_seenEvents;

		const char* DeviceName(int a_device)
		{
			switch (static_cast<RE::INPUT_DEVICE>(a_device)) {
			case RE::INPUT_DEVICE::kKeyboard: return "keyboard";
			case RE::INPUT_DEVICE::kMouse: return "mouse";
			case RE::INPUT_DEVICE::kGamepad: return "gamepad";
			case RE::INPUT_DEVICE::kFlatVirtualKeyboard: return "virtual keyboard";
			default: return "other";
			}
		}

		// First event per device, once: tells us which devices Skyrim actually receives (Steam Input check for the DualSense).
		void LogFirst(const RE::InputEvent* a_event)
		{
			const auto device = static_cast<int>(a_event->GetDevice());
			if (device < 0 || device >= static_cast<int>(g_seenDevice.size()) || g_seenDevice[device].exchange(true)) {
				return;
			}
			const auto* id = a_event->AsIDEvent();
			SKSE::log::info("[input] first {} event: type={} user event='{}' code={:#x}", DeviceName(device),
				static_cast<int>(a_event->GetEventType()), id ? id->QUserEvent().c_str() : "", id ? id->GetIDCode() : 0u);
		}

		class Sink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static Sink* Get()
			{
				static Sink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				const auto* events = RE::UserEvents::GetSingleton();
				for (auto* e = a_event ? *a_event : nullptr; e; e = e->next) {
					LogFirst(e);
					if (const auto* id = e->AsIDEvent(); id && g_seenEvents.size() < 64) {
						std::string name = id->QUserEvent().c_str();
						if (!name.empty() && std::ranges::find(g_seenEvents, name) == g_seenEvents.end()) {
							SKSE::log::info("[input] user event '{}' first seen: {} code={:#x}", name, DeviceName(static_cast<int>(e->GetDevice())), id->GetIDCode());
							g_seenEvents.push_back(std::move(name));
						}
					}
					if (!events || e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
						continue;
					}
					const auto* button = e->AsButtonEvent();
					if (e->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() < g_key.size()) {
						g_key[button->GetIDCode()].store(button->IsPressed(), std::memory_order_relaxed);
					}
					if (e->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == kToggleKey && button->IsDown()) {
						g_bridgeOn.store(!g_bridgeOn.load(std::memory_order_relaxed), std::memory_order_relaxed);
					}
					const auto& name = button->QUserEvent();
					if (name == events->sprint) {
						g_sprint.store(button->IsPressed(), std::memory_order_relaxed);
						g_sprintDevice.store(static_cast<int>(e->GetDevice()), std::memory_order_relaxed);
					}
					const RE::BSFixedString* moves[] = { &events->forward, &events->back, &events->strafeLeft, &events->strafeRight };
					for (std::size_t i = 0; i < g_move.size(); ++i) {
						if (name == *moves[i]) {
							g_move[i].store(button->IsPressed(), std::memory_order_relaxed);
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		static bool installed = false;
		if (installed) {
			return;
		}
		auto* devices = RE::BSInputDeviceManager::GetSingleton();
		if (!devices) {
			SKSE::log::error("[input] BSInputDeviceManager missing; no input capture");
			return;
		}
		devices->AddEventSink(Sink::Get());
		installed = true;
	}

	bool BridgeOn() { return g_bridgeOn.load(std::memory_order_relaxed); }

	bool SprintHeld() { return g_sprint.load(std::memory_order_relaxed); }

	bool KeyHeld(std::uint32_t a_scanCode) { return a_scanCode < g_key.size() && g_key[a_scanCode].load(std::memory_order_relaxed); }

	const char* SprintDevice() { return DeviceName(g_sprintDevice.load(std::memory_order_relaxed)); }

	Move MoveAxes()
	{
		const auto held = [](std::size_t i) { return g_move[i].load(std::memory_order_relaxed) ? 1.0f : 0.0f; };
		return { held(3) - held(2), held(0) - held(1) };
	}
}
