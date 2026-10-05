#include "bridge/PadScript.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace sxer::pad::script
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::mutex g_lock;
		std::vector<Event> g_events;
		Clock::time_point g_start;
		bool g_playing = false;

		// Polls the file every 200 ms; a new write time = a new script, played once from now.
		void Watch(std::filesystem::path a_path)
		{
			std::filesystem::file_time_type seen{};
			if (std::error_code ec; std::filesystem::exists(a_path, ec)) {
				seen = std::filesystem::last_write_time(a_path, ec);  // an old script from the last run isn't replayed
			}
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				std::error_code ec;
				const auto time = std::filesystem::last_write_time(a_path, ec);
				if (ec || time == seen) {
					continue;
				}
				seen = time;
				std::ifstream in(a_path);
				std::stringstream text;
				text << in.rdbuf();
				std::string error;
				auto events = Parse(text.str(), &error);
				if (!events) {
					SKSE::log::warn("[padscript] {} ({})", error, a_path.string());
					continue;
				}
				SKSE::log::info("[padscript] playing {} ms: {}", LengthMs(*events), text.str());
				std::lock_guard lock(g_lock);
				g_events = std::move(*events);
				g_start = Clock::now();
				g_playing = true;
			}
		}
	}

	void Start()
	{
		const char* path = std::getenv("SKYRIMXER_PADSCRIPT");
		if (!path || !*path) {
			return;
		}
		static std::once_flag once;
		std::call_once(once, [p = std::filesystem::path(path)] {
			SKSE::log::info("[padscript] dev virtual pad on: watching {}", p.string());
			std::thread(Watch, p).detach();
		});
	}

	std::optional<DsState> Overlay()
	{
		std::lock_guard lock(g_lock);
		if (!g_playing) {
			return std::nullopt;
		}
		const auto ms = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count());
		if (ms > LengthMs(g_events)) {
			g_playing = false;
			SKSE::log::info("[padscript] done");
			return std::nullopt;
		}
		return At(g_events, ms);
	}
}
