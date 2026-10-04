#include "bridge/Bridge.h"

#include "bridge/Input.h"
#include "bridge/Link.h"
#include "bridge/PlayerWatch.h"
#include "bridge/Slot.h"
#include "hooks/PlayerUpdate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace sxer::bridge
{
	namespace
	{
		std::mutex g_lock;
		// Deliberately leaked: destroying it during process exit would race the (already killed) link thread.
		// Leaking it also keeps g_link->Shared() (and the mapped region) valid for the game thread forever.
		Link* g_link = nullptr;
		// Game frames run so far (PlayerCharacter::Update calls); sent in Heartbeat events.
		std::atomic<std::uint64_t> g_frames{ 0 };

		constexpr std::uint32_t kDodge = 1u << static_cast<std::uint32_t>(proto::Button::Dodge);
		constexpr std::uint64_t kReportMs = 5000;

		// Main-thread state of OnFrame (only ever touched from PlayerCharacter::Update).
		struct FrameState
		{
			std::optional<SlotWriter<proto::InputState>> input;
			std::optional<SlotReader<proto::PlayerState>> player;
			PlayerWatch watch;
			bool held = false;
			std::vector<float> frameMs;
			std::vector<float> hookUs;
			std::uint64_t nextReportMs = 0;
		} g_frame;

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

		float Percentile(std::vector<float>& a_values, double a_p)
		{
			if (a_values.empty()) {
				return 0;
			}
			const auto k = static_cast<std::size_t>(a_p * static_cast<double>(a_values.size() - 1));
			std::nth_element(a_values.begin(), a_values.begin() + static_cast<std::ptrdiff_t>(k), a_values.end());
			return a_values[k];
		}

		void Report(FrameState& a_f, std::uint64_t a_frame, bool a_connected)
		{
			const auto n = a_f.frameMs.size();
			const auto p50 = Percentile(a_f.frameMs, 0.50), p95 = Percentile(a_f.frameMs, 0.95), p99 = Percentile(a_f.frameMs, 0.99);
			const auto hook99 = Percentile(a_f.hookUs, 0.99), hookMax = a_f.hookUs.empty() ? 0.f : *std::max_element(a_f.hookUs.begin(), a_f.hookUs.end());
			SKSE::log::info("[perf] frame={} n={} frame_ms p50={:.2f} p95={:.2f} p99={:.2f} | our hook us p99={:.1f} max={:.1f}", a_frame, n, p50, p95,
				p99, hook99, hookMax);
			if (const auto& s = a_f.watch.Last()) {
				SKSE::log::info("[state] sample: connected={} sprint={} | ER stamina={}/{} hp={}/{} fp={}/{} flags={:#x} anim={} er_frame={}", a_connected,
					a_f.held, s->stamina, s->max_stamina, s->hp, s->max_hp, s->fp, s->max_fp, s->flags, s->anim_id, s->frame);
			} else {
				SKSE::log::info("[state] sample: connected={} sprint={} | no fresh PlayerState", a_connected, a_f.held);
			}
			a_f.frameMs.clear();
			a_f.hookUs.clear();
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
					g_link->Tick(NowMs(), g_frames.load(std::memory_order_relaxed));
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(proto::kLinkTickMs));
			}
		}).detach();
		SKSE::log::info("[core] link thread started (tick {} ms); waiting for ER", proto::kLinkTickMs);

		hooks::InstallPlayerUpdate();
		input::Install();
		SKSE::log::info("[core] hooks installed: PlayerCharacter::Update (vfunc 0xAD) → InputState/PlayerState slots, input sink (Sprint → Dodge)");
	}

	void OnFrame(float a_delta)
	{
		const auto start = std::chrono::steady_clock::now();
		auto& f = g_frame;
		const auto frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
		const auto now = NowMs();
		// g_link is set before the hook is installed and never changes, so no lock is needed (and none may be taken here).
		const auto& shared = g_link->Shared();
		if (!f.input) {
			auto* base = shared.base.load(std::memory_order_acquire);
			if (!base) {
				return;  // the link hasn't joined the region yet (it retries every second)
			}
			f.input.emplace(base, proto::kOffSlotInput);
			f.player.emplace(base, proto::kOffSlotPlayer);
			f.nextReportMs = now + kReportMs;
			SKSE::log::info("[core] slots ready (frame={})", frame);
		}
		const bool connected = shared.connected.load(std::memory_order_acquire);

		const bool held = input::SprintHeld();
		if (held != f.held) {
			f.held = held;
			SKSE::log::info("[input] Dodge {} frame={} (Sprint from {})", held ? "down" : "up", frame, input::SprintDevice());
		}
		proto::InputState state{};
		state.frame = frame;
		state.time_ms = now;
		state.buttons = held ? kDodge : 0;
		f.input->Write(state);
		f.watch.Update(f.player->Read(), connected, now);

		f.frameMs.push_back(a_delta * 1000.0f);
		f.hookUs.push_back(std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count());
		if (now >= f.nextReportMs) {
			f.nextReportMs = now + kReportMs;
			Report(f, frame, connected);
		}
	}
}
