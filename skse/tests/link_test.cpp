// skyrimxer_link_test: runs the plugin's C++ Link (skse/src/bridge/Link.cpp) outside Skyrim.
//
//   skyrimxer_link_test selftest
//       Two Links (Skyrim + ER) in this process on a private region, simulated clock: handshake, heartbeat events,
//       timeout, reconnect, Bye. Exit 0 = pass. Run by ctest and tests/run-tests.ps1.
//   skyrimxer_link_test peer [--side skyrim|er] [--seconds N] [--no-bye] [--region NAME]
//       Real-time peer, for cross-language tests against `cargo run -p fake-peer`.
//
// The generated header's static_asserts also make this target the C++ layout test.

#include "bridge/Link.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <Windows.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

namespace
{
	using sxer::Link;
	using sxer::PeerStatus;
	using sxer::Side;

	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		std::printf("  %s %s\n", a_ok ? "ok  " : "FAIL", a_what);
		g_failures += a_ok ? 0 : 1;
	}

	int SelfTest()
	{
		std::ostringstream lines;
		auto logger = std::make_shared<spdlog::logger>("selftest", std::make_shared<spdlog::sinks::ostream_sink_mt>(lines));
		logger->set_pattern("%l %v");
		spdlog::set_default_logger(logger);
		const auto has = [&](const char* a_text) { return lines.str().find(a_text) != std::string::npos; };

		const auto region = L"Local\\SkyrimXER_test_cpp_" + std::to_wstring(::GetCurrentProcessId());
		Link sky(Side::Skyrim, { { 0, 1, 0 }, { 1, 7, 104, 0 } }, region);
		{
			Link er(Side::EldenRing, { { 0, 1, 0 }, { 2, 7, 1, 0 } }, region);
			const auto both = [&](std::uint64_t a_t) {
				sky.Tick(a_t, 0);
				er.Tick(a_t, 0);
			};
			for (std::uint64_t t = 1000; t <= 1100; t += 50) {
				both(t);
			}
			Check(sky.Connected() && er.Connected(), "handshake: both connected");
			Check(has("CONNECTED: handshake ok with ER") && has("CONNECTED: handshake ok with Skyrim"), "handshake logged both ways");
			Check(!has("Hello again"), "exactly one Hello each way");

			for (std::uint64_t t = 1150; t <= 6250; t += 50) {
				both(t);
			}
			Check(has("ER heartbeat seq=") && has("Skyrim heartbeat seq="), "heartbeat events received both ways");

			sky.Tick(6250 + sxer::proto::kHeartbeatTimeoutMs, 0);
			Check(sky.Connected(), "no timeout at exactly the limit");
			sky.Tick(6251 + sxer::proto::kHeartbeatTimeoutMs, 0);
			Check(sky.Status() == PeerStatus::Lost && has("heartbeat timeout"), "ER stops beating → Skyrim times out");

			for (std::uint64_t t = 9000; t <= 9100; t += 50) {
				both(t);
			}
			Check(sky.Connected() && er.Connected(), "ER resumes → reconnected");
			Check(!has("seq gap"), "no seq gaps");
		}  // ER link destroyed: Bye + handle closed
		sky.Tick(9150, 0);
		Check(sky.Status() == PeerStatus::Lost && has("said Bye (reason=Quit)"), "ER Bye → Skyrim goes idle");

		std::printf("%s", g_failures ? lines.str().c_str() : "");
		std::printf("selftest: %s\n", g_failures ? "FAILED" : "passed");
		return g_failures ? 1 : 0;
	}

	[[noreturn]] void Usage()
	{
		std::fprintf(stderr, "usage: skyrimxer_link_test selftest | peer [--side skyrim|er] [--seconds N] [--no-bye] [--region NAME]\n");
		std::exit(2);
	}

	int Peer(int a_argc, char** a_argv)
	{
		Side side = Side::Skyrim;
		double seconds = -1;
		bool bye = true;
		std::wstring region = sxer::proto::kRegionName;
		for (int i = 2; i < a_argc; ++i) {
			const std::string arg = a_argv[i];
			const auto next = [&] { return i + 1 < a_argc ? std::string(a_argv[++i]) : (Usage(), std::string()); };
			if (arg == "--side") {
				const auto v = next();
				side = v == "er" ? Side::EldenRing : v == "skyrim" ? Side::Skyrim : (Usage(), Side::Skyrim);
			} else if (arg == "--seconds") {
				seconds = std::atof(next().c_str());
			} else if (arg == "--no-bye") {
				bye = false;
			} else if (arg == "--region") {
				const auto v = next();
				region.assign(v.begin(), v.end());
			} else {
				Usage();
			}
		}
		const char* tag = side == Side::Skyrim ? "CPP-SKY" : "CPP-ER";
		auto logger = std::make_shared<spdlog::logger>("peer", std::make_shared<spdlog::sinks::stdout_sink_mt>());
		logger->set_pattern(std::string("%Y-%m-%dT%H:%M:%S.%eZ [") + tag + "] [%l] %v", spdlog::pattern_time_type::utc);
		logger->flush_on(spdlog::level::trace);
		spdlog::set_default_logger(logger);

		using Version4 = std::array<std::uint16_t, 4>;
		const sxer::Identity identity{ { 0, 1, 0 }, side == Side::Skyrim ? Version4{ 1, 7, 104, 0 } : Version4{ 2, 7, 1, 0 } };
		auto link = std::make_unique<Link>(side, identity, region);
		const auto start = std::chrono::steady_clock::now();
		std::uint64_t frames = 0;
		while (seconds < 0 || std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
			link->Tick(sxer::NowMs(), ++frames);
			std::this_thread::sleep_for(std::chrono::milliseconds(sxer::proto::kLinkTickMs));
		}
		if (!bye) {
			spdlog::info("[link] exiting without Bye (simulated crash)");
			std::fflush(stdout);
			::TerminateProcess(::GetCurrentProcess(), 0);  // skip ~Link, which would send Bye
		}
		link.reset();
		return 0;
	}
}

int main(int a_argc, char** a_argv)
{
	if (a_argc >= 2 && std::string(a_argv[1]) == "selftest") {
		return SelfTest();
	}
	if (a_argc >= 2 && std::string(a_argv[1]) == "peer") {
		return Peer(a_argc, a_argv);
	}
	Usage();
}
