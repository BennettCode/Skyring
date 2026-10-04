#include "bridge/AnimProbe.h"

#include <atomic>
#include <string_view>

// Actor::AddAnimationGraphEventSink (CommonLib, no Address Library id; skips sinks already registered).
namespace sxer::animprobe
{
	namespace
	{
		constexpr std::uint32_t kMaxLines = 3000;
		constexpr std::uint64_t kRegisterEvery = 60;
		std::atomic<std::uint32_t> g_lines{ 0 };
		std::atomic<std::uint64_t> g_frame{ 0 };

		class Sink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			static Sink* Get()
			{
				static Sink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (!a_event || g_lines.load(std::memory_order_relaxed) >= kMaxLines) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string_view tag = a_event->tag.c_str();
				if (tag == "FootLeft" || tag == "FootRight" || tag == "FootSprintLeft" || tag == "FootSprintRight" || tag == "FootFront" || tag == "FootBack") {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto n = g_lines.fetch_add(1, std::memory_order_relaxed) + 1;
				SKSE::log::info("[anim] '{}' payload='{}' frame={}", tag, a_event->payload.c_str(), g_frame.load(std::memory_order_relaxed));
				if (n == kMaxLines) {
					SKSE::log::info("[anim] line limit reached; animation event probe stops");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Update(RE::PlayerCharacter* a_player, std::uint64_t a_frame)
	{
		g_frame.store(a_frame, std::memory_order_relaxed);
		if (a_player && a_frame % kRegisterEvery == 0 && g_lines.load(std::memory_order_relaxed) < kMaxLines) {
			if (a_player->AddAnimationGraphEventSink(Sink::Get())) {
				SKSE::log::info("[anim] event probe registered on the player's graph frame={}", a_frame);
			}
		}
	}
}
