#include "bridge/Bridge.h"

#include "bridge/Hud.h"
#include "bridge/Input.h"
#include "bridge/Link.h"
#include "bridge/PlayerWatch.h"
#include "bridge/Slot.h"
#include "hooks/PlayerUpdate.h"
#include "hooks/SprintSwallow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
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
		// See SwallowSprint().
		std::atomic<bool> g_swallowSprint{ false };

		constexpr std::uint32_t kDodge = 1u << static_cast<std::uint32_t>(proto::Button::Dodge);
		constexpr std::uint32_t kInWorld = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::InWorld);
		constexpr std::uint64_t kReportMs = 5000;
		// How long after a Dodge press the HUD summary waits for ER's reaction (ER starts a backstep ~9 frames after the press).
		constexpr std::uint64_t kDodgeReportMs = 600;
		// Coordinate test probe: one sample every kCoordsEvery frames while moving, at most kCoordsMaxLines per session.
		constexpr std::uint64_t kCoordsEvery = 6;
		constexpr std::uint32_t kCoordsMaxLines = 1500;

		// What ER did with one Dodge press, for the HUD summary.
		struct DodgeReport
		{
			std::uint64_t startMs = 0;
			std::int32_t anim0 = 0;
			std::int32_t stamina0 = 0;
			std::int32_t staminaMin = 0;
			std::int32_t firstNewAnim = 0;
			bool animChanged = false;
			bool iframe = false;
		};

		// Main-thread state of OnFrame (only ever touched from PlayerCharacter::Update).
		struct FrameState
		{
			std::optional<SlotWriter<proto::InputState>> input;
			std::optional<SlotReader<proto::PlayerState>> player;
			PlayerWatch watch;
			bool bridgeOn = true;
			bool swallow = false;
			bool held = false;
			input::Move move;
			std::vector<float> frameMs;
			std::vector<float> hookUs;
			std::uint64_t nextReportMs = 0;
			// HUD feedback state.
			bool hudConnected = false;
			bool hudInWorld = false;
			std::optional<DodgeReport> dodge;
			// Coordinate probe.
			RE::NiPoint3 coordsLast;
			std::uint32_t coordsLines = 0;
		} g_frame;

		// Coordinate test (docs/research/coordinates.md): position (Z-up, game units) and heading (data.angle.z, radians) every
		// kCoordsEvery frames while the player moved more than 1 unit since the last sample or a movement key is held. Bounded.
		void SampleCoords(FrameState& a_f, const RE::PlayerCharacter* a_player, std::uint64_t a_frame, const input::Move& a_move)
		{
			if (!a_player || a_f.coordsLines >= kCoordsMaxLines || a_frame % kCoordsEvery != 0) {
				return;
			}
			const auto pos = a_player->GetPosition();
			const bool moving = a_move.x != 0 || a_move.y != 0;
			if (!moving && pos.GetDistance(a_f.coordsLast) <= 1.0f) {
				return;
			}
			a_f.coordsLast = pos;
			++a_f.coordsLines;
			SKSE::log::info("[coords] frame={} pos=({:.1f},{:.1f},{:.1f}) heading={:.4f} move={},{}", a_frame, pos.x, pos.y, pos.z,
				a_player->GetAngleZ(), a_move.x, a_move.y);
		}

		// Playtest feedback in Skyrim's HUD: link and ER-world changes, and one summary per Dodge press.
		void UpdateHud(FrameState& a_f, bool a_connected, bool a_pressed, std::uint64_t a_now)
		{
			const auto& s = a_f.watch.Last();
			if (a_connected != a_f.hudConnected) {
				a_f.hudConnected = a_connected;
				hud::Notify(a_connected ? "SkyrimXER: Elden Ring connected" : "SkyrimXER: Elden Ring link lost");
			}
			const bool inWorld = s && (s->flags & kInWorld);
			if (a_connected && inWorld != a_f.hudInWorld) {
				a_f.hudInWorld = inWorld;
				hud::Notify(inWorld ? "SkyrimXER: ER character is in the world" : "SkyrimXER: ER character not in the world (title/loading)");
			}
			if (a_pressed) {
				if (!s) {
					hud::Notify("SkyrimXER: dodge pressed, but there is no ER state (not connected?)");
				} else {
					a_f.dodge = DodgeReport{ a_now, s->anim_id, s->stamina, s->stamina, 0, false, false };
				}
			}
			if (!a_f.dodge) {
				return;
			}
			auto& d = *a_f.dodge;
			if (s) {
				d.staminaMin = std::min(d.staminaMin, s->stamina);
				d.iframe |= (s->flags & PlayerWatch::kIFrame) != 0;
				if (!d.animChanged && s->anim_id != d.anim0) {
					d.animChanged = true;
					d.firstNewAnim = s->anim_id;
				}
			}
			if (a_now - d.startMs >= kDodgeReportMs) {
				const auto what = d.animChanged ? std::format("anim {}", d.firstNewAnim) : std::format("no reaction (anim stayed {})", d.anim0);
				hud::Notify(std::format("ER dodge: {} | stamina {}->{} | i-frames {}", what, d.stamina0, d.staminaMin, d.iframe ? "yes" : "no"));
				a_f.dodge.reset();
			}
		}

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
				SKSE::log::info("[state] sample: connected={} sprint={} move={},{} | ER stamina={}/{} hp={}/{} fp={}/{} flags={:#x} anim={} er_frame={}",
					a_connected, a_f.held, a_f.move.x, a_f.move.y, s->stamina, s->max_stamina, s->hp, s->max_hp, s->fp, s->max_fp, s->flags, s->anim_id, s->frame);
			} else {
				SKSE::log::info("[state] sample: connected={} sprint={} move={},{} | no fresh PlayerState", a_connected, a_f.held, a_f.move.x, a_f.move.y);
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
		hooks::InstallSprintSwallow();
		input::Install();
		SKSE::log::info("[core] hooks installed: PlayerCharacter::Update (vfunc 0xAD) → InputState/PlayerState slots, SprintHandler::CanProcess "
		                "(vfunc 0x1, vanilla sprint off while bridged), input sink (Sprint → Dodge, movement keys → move stick, F10 toggle)");
	}

	bool SwallowSprint() { return g_swallowSprint.load(std::memory_order_relaxed); }

	void OnFrame(const RE::PlayerCharacter* a_player, float a_delta)
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

		const bool on = input::BridgeOn();
		if (on != f.bridgeOn) {
			f.bridgeOn = on;
			hud::Notify(on ? "SkyrimXER: bridge ON (Sprint = Elden Ring dodge), F10 toggles" : "SkyrimXER: bridge OFF (vanilla Skyrim), F10 toggles");
		}
		const auto& last = f.watch.Last();
		const bool swallow = on && connected && last && (last->flags & kInWorld);
		if (swallow != f.swallow) {
			f.swallow = swallow;
			g_swallowSprint.store(swallow, std::memory_order_relaxed);
			SKSE::log::info("[input] vanilla Sprint {} frame={}", swallow ? "swallowed (Sprint = ER dodge only)" : "restored", frame);
		}
		const bool held = on && input::SprintHeld();
		const bool pressed = held && !f.held;
		if (held != f.held) {
			f.held = held;
			SKSE::log::info("[input] Dodge {} frame={} (Sprint from {})", held ? "down" : "up", frame, input::SprintDevice());
		}
		const auto move = on ? input::MoveAxes() : input::Move{};
		if (move != f.move) {
			f.move = move;
			SKSE::log::info("[input] move x={} y={} frame={}", move.x, move.y, frame);
		}
		proto::InputState state{};
		state.frame = frame;
		state.time_ms = now;
		state.buttons = held ? kDodge : 0;
		state.move_x = move.x;
		state.move_y = move.y;
		f.input->Write(state);
		SampleCoords(f, a_player, frame, move);
		f.watch.Update(f.player->Read(), connected, now);
		UpdateHud(f, connected, pressed, now);

		f.frameMs.push_back(a_delta * 1000.0f);
		f.hookUs.push_back(std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count());
		if (now >= f.nextReportMs) {
			f.nextReportMs = now + kReportMs;
			Report(f, frame, connected);
		}
	}
}
