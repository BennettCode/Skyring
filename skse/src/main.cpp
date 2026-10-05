#include "bridge/AutoLoad.h"
#include "bridge/Bridge.h"

namespace
{
	// Line format shared with the ER plugin so logs from both sides can be merged by timestamp (docs/DESIGN.md, "Logging format").
	constexpr auto kLogPattern = "%Y-%m-%dT%H:%M:%S.%eZ [SKY] [%l] %v";

	void SetupLog()
	{
		const auto logDir = SKSE::log::log_directory();
		if (!logDir) {
			SKSE::stl::report_and_fail("SkyrimXER: SKSE log directory not found");
		}

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*logDir / "SkyrimXER.log").string(), true);
		auto logger = std::make_shared<spdlog::logger>("SkyrimXER", std::move(sink));
		logger->set_pattern(kLogPattern, spdlog::pattern_time_type::utc);
		logger->set_level(spdlog::level::trace);
		logger->flush_on(spdlog::level::trace);
		spdlog::set_default_logger(std::move(logger));
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			SKSE::log::info("[core] kDataLoaded: game data ready (main menu)");
			sxer::bridge::Start();
			sxer::autoload::Install();
			break;
		case SKSE::MessagingInterface::kNewGame:
			SKSE::log::info("[core] new game started");
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			SKSE::log::info("[core] save loaded (success={})", static_cast<bool>(a_msg->data));
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	// CommonLib's own logger would replace ours (local time, different line format), so turn it off.
	SKSE::Init(a_skse, SKSE::InitInfo{ .log = false });
	SetupLog();
	// Call hooks (hooks/PlayerHit) write a 5-byte call to a trampoline stub.
	SKSE::AllocTrampoline(64);

	const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
	SKSE::log::info("[core] {} v{} loaded, runtime {}", plugin->GetName(), plugin->GetVersion().string("."sv),
		REL::Module::get().version().string("."sv));

	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		SKSE::log::critical("[core] failed to register SKSE message listener");
		return false;
	}
	return true;
}
