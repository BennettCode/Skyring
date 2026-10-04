#include "bridge/VanillaInput.h"

// SneakHandler: PlayerControls singleton (AE 400864, checked) → sneakHandler, called through its own vtable (CanProcess vfunc 0x1,
// ProcessButton vfunc 0x4), no Address Library id of its own. ButtonEvent::Create = game heap + VTABLE_ButtonEvent (AE 208708);
// UserEvents singleton AE 402638; Actor::HasPerk AE 37698; AddPerk/RemovePerk = Actor vfuncs 0xFB/0xFC. All ids checked with
// tools/addrlib-check.ps1 (docs/research/skyrim-hooks.md).
namespace sxer::vanilla
{
	namespace
	{
		constexpr RE::FormID kSilentRoll = 0x00105F23;
		// Left Ctrl, Skyrim's default Sneak key (only used as the event's id code).
		constexpr std::uint32_t kSneakKey = 0x1D;
		constexpr std::uint64_t kPerkCheckFrames = 60;

		bool g_added = false;
		std::uint64_t g_lastCheck = 0;

		RE::BGSPerk* RollPerk()
		{
			static auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(kSilentRoll);
			return perk;
		}
	}

	bool PressSneak()
	{
		auto* controls = RE::PlayerControls::GetSingleton();
		auto* events = RE::UserEvents::GetSingleton();
		if (!controls || !controls->sneakHandler || !events) {
			return false;
		}
		auto* event = RE::ButtonEvent::Create(RE::INPUT_DEVICE::kKeyboard, events->sneak, kSneakKey, 1.0f, 0.0f);
		if (!event) {
			return false;
		}
		auto* handler = static_cast<RE::PlayerInputHandler*>(controls->sneakHandler);
		const bool can = handler->CanProcess(event);
		if (can) {
			handler->ProcessButton(event, std::addressof(controls->data));
		}
		RE::free(event);
		return can;
	}

	bool HasRollPerk(RE::PlayerCharacter* a_player)
	{
		auto* perk = RollPerk();
		return a_player && perk && a_player->HasPerk(perk);
	}

	void UpdateRollPerk(RE::PlayerCharacter* a_player, bool a_on, std::uint64_t a_frame)
	{
		auto* perk = RollPerk();
		if (!a_player || !perk) {
			return;
		}
		if (!a_on) {
			if (g_added) {
				g_added = false;
				a_player->RemovePerk(perk);
				SKSE::log::info("[move] Silent Roll perk removed again (bridge off) frame={}", a_frame);
			}
			return;
		}
		if (a_frame - g_lastCheck < kPerkCheckFrames) {
			return;
		}
		g_lastCheck = a_frame;
		if (!a_player->HasPerk(perk)) {
			a_player->AddPerk(perk);
			g_added = true;
			SKSE::log::info("[move] Silent Roll perk added (bridge on; the vanilla roll animation needs it): has={} frame={}",
				a_player->HasPerk(perk), a_frame);
		}
	}
}
