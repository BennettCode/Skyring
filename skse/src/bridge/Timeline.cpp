#include "bridge/Timeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <utility>

#include "bridge/Rig.h"

// Adapted from SkyCraft skse/src/Game.cpp:662-760 (MIT, Copyright (c) chasmlol; licenses/SkyCraft.txt): history of stamped samples,
// render slightly in the past with a delay that follows how late samples arrive, pure interpolation. ER's frames aren't a fixed
// rhythm like Minecraft's 20 Hz ticks, so the rhythm locking is left out; the QPC stamps (time_us) are taken as they come.
namespace sxer::timeline
{
	namespace
	{
		constexpr std::size_t kHistory = 12;     // ~200 ms of ER frames
		constexpr std::size_t kDueWindow = 120;  // ~2 s of arrivals for the delay
		constexpr double kMinDelayMs = 4.0, kMaxDelayMs = 40.0;
		constexpr std::uint64_t kGapUs = 250'000;  // longer than this between samples = a break (pause, load): restart

		template <class T>
		struct History
		{
			std::deque<T> items;

			// Adds a sample if it's new (by time_us); returns true then. A stamp going backwards = the writer restarted.
			bool Add(const T& a_s)
			{
				if (a_s.time_us == 0) {
					return false;
				}
				if (!items.empty()) {
					if (a_s.time_us == items.back().time_us) {
						return false;
					}
					if (a_s.time_us < items.back().time_us || a_s.time_us - items.back().time_us > kGapUs) {
						items.clear();
					}
				}
				items.push_back(a_s);
				if (items.size() > kHistory) {
					items.pop_front();
				}
				return true;
			}

			// The two samples around a_t (a ≤ t < b) and how far between them (0..1); b = nullptr when t is at/after the newest.
			std::pair<const T*, const T*> Around(std::uint64_t a_t, float& a_u) const
			{
				a_u = 0;
				if (items.empty()) {
					return { nullptr, nullptr };
				}
				if (a_t <= items.front().time_us) {
					return { &items.front(), nullptr };
				}
				for (std::size_t i = items.size(); i-- > 0;) {
					if (items[i].time_us <= a_t) {
						if (i + 1 < items.size()) {
							const auto& b = items[i + 1];
							a_u = static_cast<float>(double(a_t - items[i].time_us) / double(b.time_us - items[i].time_us));
							return { &items[i], &b };
						}
						return { &items[i], nullptr };
					}
				}
				return { &items.front(), nullptr };
			}
		};

		struct State
		{
			History<proto::PlayerState> player;
			History<proto::PoseState> pose;
			double delayMs = 12.0;
			std::array<double, kDueWindow> due{};
			std::size_t dueNext = 0;
			bool dueInit = false;
			std::uint64_t lastFrameUs = 0;
			std::uint32_t late = 0;
		};
		State g;

		float LerpAngle(float a_a, float a_b, float a_u)
		{
			return a_a + std::remainder(a_b - a_a, 2.0f * 3.14159265f) * a_u;
		}
	}

	void Push(const std::optional<proto::PlayerState>& a_player, const std::optional<proto::PoseState>& a_pose, std::uint64_t a_nowUs)
	{
		if (a_player) {
			g.player.Add(*a_player);
		}
		if (a_pose) {
			g.pose.Add(*a_pose);
		}
		// Interpolating needs the render time (now - delay) to stay at or before the newest sample, i.e. delay >= the newest sample's
		// age. Its age, every frame: it grows until ER's next sample turns up (SkyCraft measured lateness against Minecraft's fixed tick
		// rhythm; ER's frames have no fixed rhythm, so the age itself is the measure). 2026-10-04: the lateness measure gave 4-6 ms and
		// most frames ran past the newest sample.
		const std::uint64_t newest = std::max(g.player.items.empty() ? 0 : g.player.items.back().time_us,
			g.pose.items.empty() ? 0 : g.pose.items.back().time_us);
		if (newest != 0 && a_nowUs >= newest) {
			if (!g.dueInit) {
				g.due.fill(g.delayMs - 1.0);
				g.dueInit = true;
			}
			const double ageMs = (double(a_nowUs) - double(newest)) / 1000.0;
			if (ageMs < 50.0) {  // older than that is a hitch (ER paused), not a pattern to wait for
				g.due[g.dueNext++ % g.due.size()] = ageMs;
			}
		}
		// The delay grows 2% slower than real time and shrinks 0.2% faster: too little to see either way (SkyCraft's rates).
		const double frameMs = g.lastFrameUs != 0 ? (double(a_nowUs) - double(g.lastFrameUs)) / 1000.0 : 0.0;
		g.lastFrameUs = a_nowUs;
		if (g.dueInit) {
			const double target = std::clamp(*std::ranges::max_element(g.due) + 1.0, kMinDelayMs, kMaxDelayMs);
			const double dt = std::min(frameMs, 100.0) / 1000.0;
			g.delayMs = target > g.delayMs ? std::min(target, g.delayMs + 20.0 * dt) : std::max(target, g.delayMs - 2.0 * dt);
		}
	}

	View At(std::uint64_t a_nowUs)
	{
		View v;
		const auto render = a_nowUs - static_cast<std::uint64_t>(g.delayMs * 1000.0);
		float u = 0;
		if (const auto [a, b] = g.player.Around(render, u); a) {
			auto s = *a;
			if (b) {
				for (int i = 0; i < 3; ++i) {
					s.pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * u;
				}
				s.yaw = LerpAngle(a->yaw, b->yaw, u);
				s.cam_yaw = LerpAngle(a->cam_yaw, b->cam_yaw, u);
			} else if (render > a->time_us) {
				v.late = true;
			}
			s.time_us = render;
			v.player = s;
		}
		if (const auto [a, b] = g.pose.Around(render, u); a) {
			auto p = *a;
			if (b) {
				for (std::size_t k = 0; k < proto::kPoseBoneCount; ++k) {
					const rig::Quat qa{ a->rot[k * 4], a->rot[k * 4 + 1], a->rot[k * 4 + 2], a->rot[k * 4 + 3] };
					const rig::Quat qb{ b->rot[k * 4], b->rot[k * 4 + 1], b->rot[k * 4 + 2], b->rot[k * 4 + 3] };
					const auto q = rig::Slerp(qa, qb, u);
					p.rot[k * 4] = q.x, p.rot[k * 4 + 1] = q.y, p.rot[k * 4 + 2] = q.z, p.rot[k * 4 + 3] = q.w;
				}
				for (int i = 0; i < 3; ++i) {
					p.pelvis_offset[i] = a->pelvis_offset[i] + (b->pelvis_offset[i] - a->pelvis_offset[i]) * u;
				}
				p.yaw = LerpAngle(a->yaw, b->yaw, u);
			}
			p.time_us = render;
			v.pose = p;
		}
		g.late += v.late ? 1 : 0;
		return v;
	}

	void Reset()
	{
		g.player.items.clear();
		g.pose.items.clear();
	}

	float DelayMs() { return static_cast<float>(g.delayMs); }

	std::uint32_t TakeLateFrames() { return std::exchange(g.late, 0u); }
}
