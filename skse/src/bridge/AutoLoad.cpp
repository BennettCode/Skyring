#include "bridge/AutoLoad.h"

#include <cstdlib>
#include <string>

// Main-menu sink → load a save through the game's own save manager, on the main thread (SKSE task).
// Address Library (AE ids, checked present with tools/addrlib-check.ps1, docs/research/skyrim-hooks.md "Dev auto-load"):
// UI singleton 400327, BGSSaveLoadManager singleton 403340, LoadMostRecentSaveGame 35766, Load 35757. All through CommonLib.
namespace sxer::autoload
{
	namespace
	{
		std::string Setting()
		{
			char* value = nullptr;
			std::size_t len = 0;
			if (_dupenv_s(&value, &len, "SKYRIMXER_AUTOLOAD") != 0 || !value) {
				return {};
			}
			std::string out(value);
			std::free(value);
			return out;
		}

		void Load(const std::string& a_save)
		{
			auto* manager = RE::BGSSaveLoadManager::GetSingleton();
			if (!manager) {
				SKSE::log::error("[autoload] no BGSSaveLoadManager; load the save by hand");
				return;
			}
			if (a_save == "1") {
				const bool ok = manager->LoadMostRecentSaveGame();
				SKSE::log::info("[autoload] loading the most recent save: {}", ok ? "started" : "FAILED (no saves?)");
			} else {
				SKSE::log::info("[autoload] loading save '{}'", a_save);
				manager->Load(a_save.c_str(), false);
			}
		}

		class MainMenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			explicit MainMenuSink(std::string a_save) :
				save_(std::move(a_save))
			{}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!done_ && a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME) {
					done_ = true;
					SKSE::log::info("[autoload] main menu open; SKYRIMXER_AUTOLOAD={}", save_);
					// Not from inside the menu event: queue it for the main thread's task pass.
					const auto save = save_;
					SKSE::GetTaskInterface()->AddTask([save] { Load(save); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			std::string save_;
			bool done_ = false;
		};
	}

	void Install()
	{
		auto save = Setting();
		if (save.empty()) {
			return;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			SKSE::log::error("[autoload] no UI singleton; load the save by hand");
			return;
		}
		static MainMenuSink sink(std::move(save));
		ui->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
		SKSE::log::info("[autoload] armed (dev launch): will load a save when the main menu opens");
	}
}
