#include "hooks/XInput.h"

#include "bridge/Gamepad.h"
#include "bridge/Input.h"
#include "bridge/PadScript.h"

#include "skyrimxer_protocol.h"

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <set>
#include <string>

// SkyrimSE.exe imports XINPUT1_3.dll by ordinal, which CommonLib's SKSE::PatchIAT (name lookup) skips. So the import table is walked by
// hand and every entry that resolved to XInputGetState is replaced. No game code is patched and no Address Library id is used.
// A real XInput pad (an Xbox pad, or Steam Input when Skyrim was started through Steam) always wins; the DualSense fills in as user 0.
// In gameplay with the bridge on, Skyrim sees the Elden Ring layout (pad/PadReport.h) and the ER buttons go to InputState instead.
namespace sxer::hooks
{
	static_assert(pad::kErAttack == 1u << static_cast<std::uint32_t>(proto::Button::Attack));
	static_assert(pad::kErStrongAttack == 1u << static_cast<std::uint32_t>(proto::Button::StrongAttack));
	static_assert(pad::kErGuard == 1u << static_cast<std::uint32_t>(proto::Button::Guard));
	static_assert(pad::kErSkill == 1u << static_cast<std::uint32_t>(proto::Button::Skill));

	namespace
	{
		using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		GetStateFn g_real = nullptr;
		std::atomic<bool> g_menuMode{ false };
		std::atomic<bool> g_loggedPad{ false };
		std::atomic<bool> g_loggedReal{ false };
		std::atomic<int> g_layout{ -1 };

		// ER buttons the pad holds right now (InputState.buttons bits), written by Skyrim's XInput poll.
		std::atomic<std::uint32_t> g_erButtons{ 0 };
		// The Journal is open (L1/R1 also switch its tabs); set by MenuWatch.
		std::atomic<bool> g_journal{ false };
		// The touchpad was pressed in gameplay: open Skyrim's map (bridge::OnFrame takes it).
		std::atomic<bool> g_mapRequest{ false };
		std::atomic<bool> g_touchHeld{ false };

		// Gameplay = the bridge is on and no menu that pauses the game or takes the pad (cursor / menu context) is open.
		bool Gameplay() { return input::BridgeOn() && !g_menuMode.load(std::memory_order_relaxed); }

		// Rewrites a_state for Skyrim in the active layout and publishes the ER buttons.
		// DualSense buttons that were down when the layout last switched: ignored until released, so the press that opened a menu
		// (Create = Journal, Options = Tween) doesn't arrive in the menu as a different button (Create = Back = Wait there).
		std::uint32_t g_heldAcrossSwitch = 0;

		void ApplyLayout(XINPUT_STATE* a_state, const pad::DsState& a_raw, std::uint32_t a_packet)
		{
			const bool gameplay = Gameplay();
			const bool switched = g_layout.load(std::memory_order_relaxed) != (gameplay ? 1 : 0);
			if (switched) {
				g_heldAcrossSwitch = a_raw.buttons;
			}
			g_heldAcrossSwitch &= a_raw.buttons;
			pad::DsState a_ds = a_raw;
			a_ds.buttons &= ~g_heldAcrossSwitch;
			if (g_heldAcrossSwitch & (pad::kL2 | pad::kR2)) {
				a_ds.l2 = (g_heldAcrossSwitch & pad::kL2) ? 0 : a_ds.l2;
				a_ds.r2 = (g_heldAcrossSwitch & pad::kR2) ? 0 : a_ds.r2;
			}
			if (g_layout.exchange(gameplay ? 1 : 0, std::memory_order_relaxed) != (gameplay ? 1 : 0)) {
				SKSE::log::info("[pad] layout: {}", gameplay ? "gameplay (Elden Ring buttons: R1/R2/L1/L2 to ER, Circle roll/sprint, Cross jump, Triangle activate)" :
				                                               "menu (plain Xbox positions; menu open or bridge off)");
			}
			const auto out = gameplay ? pad::ToXInputGameplay(a_ds) : pad::ToXInputMenu(a_ds, g_journal.load(std::memory_order_relaxed));
			const bool touch = (a_ds.buttons & pad::kTouchpad) != 0;
			if (touch != g_touchHeld.exchange(touch, std::memory_order_relaxed) && touch && gameplay) {
				g_mapRequest.store(true, std::memory_order_relaxed);
			}
			std::memcpy(&a_state->Gamepad, &out, sizeof(XINPUT_GAMEPAD));
			// A layout switch changes the buttons without a new report, so it counts as a new packet too.
			a_state->dwPacketNumber = a_packet * 2 + (gameplay ? 1 : 0);
			g_erButtons.store(gameplay ? pad::ToErButtons(a_ds) : 0, std::memory_order_relaxed);
		}

		// Packet number for our own (DualSense + virtual pad) state: bumped whenever the combined state changes.
		std::uint32_t g_ownPacket = 0;
		std::array<std::atomic<bool>, 4> g_probed{};
		std::optional<pad::DsState> g_ownLast;

		// Logs once what the other XInput pad is (Steam Input re-exposes the DualSense as an Xbox pad: it has no touchpad, so it
		// must not be Skyrim's source while we read the DualSense ourselves).
		void DescribeRealPad(DWORD a_user)
		{
			using CapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
			static const auto caps = reinterpret_cast<CapsFn>(GetProcAddress(GetModuleHandleW(L"xinput1_3.dll"), "XInputGetCapabilities"));
			XINPUT_CAPABILITIES c{};
			if (caps && caps(a_user, 0, &c) == ERROR_SUCCESS) {
				SKSE::log::info("[pad] other XInput pad on user {}: type {} subtype {} flags {:#x}", a_user, c.Type, c.SubType, c.Flags);
			}
		}

		DWORD WINAPI GetState(DWORD a_user, XINPUT_STATE* a_state)
		{
			if (!a_state) {
				return g_real ? g_real(a_user, a_state) : ERROR_DEVICE_NOT_CONNECTED;
			}
			// Our own pad first: the DualSense we read over HID, plus the dev virtual pad (bridge/PadScript). It is user 0 and the only
			// pad Skyrim sees, so a second copy of the same DualSense (Steam Input) can't double or swallow presses.
			const auto ds = pad::Latest();
			const auto overlay = pad::script::Overlay();
			if (ds || overlay) {
				// Once per user: is there another pad (logged, then hidden)? XInputGetState on an empty slot is slow, so never per frame.
				if (a_user < g_probed.size() && !g_probed[a_user].exchange(true) && g_real) {
					XINPUT_STATE other{};
					if (g_real(a_user, &other) == ERROR_SUCCESS) {
						SKSE::log::info("[pad] another XInput pad answers as user {}; hidden from Skyrim while the DualSense is read directly", a_user);
						DescribeRealPad(a_user);
					}
				}
				if (a_user != 0) {
					return ERROR_DEVICE_NOT_CONNECTED;
				}
				pad::DsState combined = ds.value_or(pad::DsState{});
				if (overlay) {
					combined.buttons |= overlay->buttons;
					combined.l2 = std::max(combined.l2, overlay->l2);
					combined.r2 = std::max(combined.r2, overlay->r2);
					if (overlay->lx || overlay->ly) combined.lx = overlay->lx, combined.ly = overlay->ly;
					if (overlay->rx || overlay->ry) combined.rx = overlay->rx, combined.ry = overlay->ry;
				}
				if (combined != g_ownLast) {
					g_ownLast = combined;
					++g_ownPacket;
				}
				*a_state = XINPUT_STATE{};
				ApplyLayout(a_state, combined, g_ownPacket);
				if (!g_loggedPad.exchange(true)) {
					SKSE::log::info("[pad] Skyrim reads the DualSense{} as XInput user 0", ds ? "" : " (virtual pad only)");
				}
				return ERROR_SUCCESS;
			}
			// No DualSense: an Xbox pad (or Steam Input's) passes through, with the same layout.
			const DWORD result = g_real ? g_real(a_user, a_state) : ERROR_DEVICE_NOT_CONNECTED;
			if (result == ERROR_SUCCESS) {
				if (!g_loggedReal.exchange(true)) {
					SKSE::log::info("[pad] no DualSense open; using the XInput pad on user {} with the same layout", a_user);
					DescribeRealPad(a_user);
				}
				pad::PadState raw;
				static_assert(sizeof(XINPUT_GAMEPAD) == sizeof(pad::PadState));
				std::memcpy(&raw, &a_state->Gamepad, sizeof(XINPUT_GAMEPAD));
				ApplyLayout(a_state, pad::FromXInput(raw), a_state->dwPacketNumber);
			} else if (a_user == 0) {
				g_erButtons.store(0, std::memory_order_relaxed);
			}
			return result;
		}

		// Patches every import thunk of a_dll in the main module whose resolved address is a_target. Returns how many.
		int PatchImports(const char* a_dll, void* a_target, void* a_replacement)
		{
			auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (dir.VirtualAddress == 0) {
				return 0;
			}
			int patched = 0;
			for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
				if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), a_dll) != 0) {
					continue;
				}
				for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk); thunk->u1.Function; ++thunk) {
					if (reinterpret_cast<void*>(thunk->u1.Function) != a_target) {
						continue;
					}
					DWORD old = 0;
					if (VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &old)) {
						thunk->u1.Function = reinterpret_cast<ULONGLONG>(a_replacement);
						VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), old, &old);
						++patched;
					}
				}
			}
			return patched;
		}

		// Tracks open menus that take the pad, by name (open/close events arrive on the main thread).
		class MenuWatch final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuWatch* Get()
			{
				static MenuWatch sink;
				return &sink;
			}

			static bool TakesPad(const RE::IMenu& a_menu) { return a_menu.PausesGame() || a_menu.UsesCursor() || a_menu.UsesMenuContext(); }

			// Menus already open when the watch starts (the main menu at kDataLoaded).
			void Seed(RE::UI* a_ui)
			{
				for (const auto& [name, entry] : a_ui->menuMap) {
					if (entry.menu && a_ui->IsMenuOpen(name) && TakesPad(*entry.menu)) {
						open_.insert(name.c_str());
					}
				}
				g_menuMode.store(!open_.empty(), std::memory_order_relaxed);
				SKSE::log::info("[pad] menus taking the pad at start: {}", open_.size());
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string name = a_event->menuName.c_str();
				if (static int lines = 0; lines++ < 300) {
					SKSE::log::info("[pad] menu {} {}", a_event->opening ? "open" : "close", name);
				}
				if (a_event->opening) {
					auto* ui = RE::UI::GetSingleton();
					const auto menu = ui ? ui->GetMenu(a_event->menuName) : nullptr;
					if (menu && TakesPad(*menu)) {
						open_.insert(name);
					}
				} else {
					open_.erase(name);
				}
				g_menuMode.store(!open_.empty(), std::memory_order_relaxed);
				if (a_event->menuName == RE::JournalMenu::MENU_NAME) {
					g_journal.store(a_event->opening, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			std::set<std::string> open_;
		};
	}

	void InstallXInput()
	{
		pad::Start();
		pad::script::Start();
		HMODULE xinput = GetModuleHandleW(L"xinput1_3.dll");
		if (!xinput) {
			xinput = LoadLibraryW(L"xinput1_3.dll");
		}
		auto* target = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
		if (!target) {
			SKSE::log::warn("[pad] xinput1_3.dll / XInputGetState not found; the DualSense won't reach Skyrim");
			return;
		}
		g_real = reinterpret_cast<GetStateFn>(target);
		const int patched = PatchImports("XINPUT1_3.dll", target, reinterpret_cast<void*>(&GetState));
		if (patched == 0) {
			SKSE::log::warn("[pad] no XInputGetState import found in SkyrimSE.exe; the DualSense won't reach Skyrim");
			return;
		}
		SKSE::log::info("[pad] XInputGetState import patched ({} entries): Skyrim sees the DualSense as an Xbox pad", patched);
	}

	std::uint32_t PadErButtons() { return g_erButtons.load(std::memory_order_relaxed); }

	bool TakeMapRequest() { return g_mapRequest.exchange(false, std::memory_order_relaxed); }

	void InstallXInputMenuWatch()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			MenuWatch::Get()->Seed(ui);
			ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatch::Get());
			SKSE::log::info("[pad] menu watch on (Circle/L1 swap only in gameplay)");
		}
	}
}
