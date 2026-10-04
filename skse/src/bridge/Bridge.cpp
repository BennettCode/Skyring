#include "bridge/Bridge.h"

#include "bridge/Link.h"

#include <chrono>
#include <mutex>
#include <thread>

namespace sxer::bridge
{
	namespace
	{
		std::mutex g_lock;
		// Deliberately leaked: destroying it during process exit would race the (already killed) link thread.
		Link* g_link = nullptr;

		// Clean exit sends Bye (best effort; a crash or TerminateProcess is covered by ER's heartbeat timeout).
		// Runs at DLL_PROCESS_DETACH, when other threads are already dead, possibly holding g_lock or the logger's
		// lock, so it never blocks and never logs.
		struct ExitGuard
		{
			~ExitGuard()
			{
				if (g_lock.try_lock()) {
					if (g_link) {
						g_link->Shutdown(false);
					}
					g_lock.unlock();
				}
			}
		} g_exitGuard;

		Identity MakeIdentity()
		{
			const auto plugin = SKSE::PluginDeclaration::GetSingleton()->GetVersion();
			const auto game = REL::Module::get().version();
			return { { plugin.major(), plugin.minor(), plugin.patch() }, { game.major(), game.minor(), game.patch(), game.build() } };
		}
	}

	void Start()
	{
		std::lock_guard lock(g_lock);
		if (g_link) {
			return;
		}
		g_link = new Link(Side::Skyrim, MakeIdentity());
		std::thread([] {
			for (;;) {
				{
					std::lock_guard tick(g_lock);
					// No per-frame hook yet (Phase 3), so frames stays 0.
					g_link->Tick(NowMs(), 0);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(proto::kLinkTickMs));
			}
		}).detach();
		SKSE::log::info("[core] link thread started (tick {} ms); waiting for ER", proto::kLinkTickMs);
	}
}
