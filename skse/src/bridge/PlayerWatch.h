#pragma once

// Logs PlayerState edges on the Skyrim side: fresh/stale transitions, stamina changes, IFrame on/off, anim changes.
// Same wording as tools/fake-peer (PlayerWatch). Used by the plugin (Bridge.cpp) and skyrimxer_link_test's peer mode.

#include <cstdint>
#include <format>
#include <optional>
#include <string>

#include <spdlog/spdlog.h>

#include "bridge/Slot.h"
#include "skyrimxer_protocol.h"

namespace sxer
{
	class PlayerWatch
	{
	public:
		static constexpr std::uint32_t kIFrame = 1u << static_cast<std::uint32_t>(proto::PlayerFlag::IFrame);

		void Update(const std::optional<proto::PlayerState>& a_state, bool a_connected, std::uint64_t a_now)
		{
			const bool fresh = a_connected && a_state && Fresh(a_state->time_ms, a_now);
			if (fresh != fresh_) {
				fresh_ = fresh;
				if (fresh) {
					const auto& s = *a_state;
					spdlog::info("[state] PlayerState fresh: stamina={}/{} hp={}/{} flags={:#x} anim={} er_frame={}", s.stamina, s.max_stamina, s.hp,
						s.max_hp, s.flags, s.anim_id, s.frame);
				} else {
					const auto why = !a_connected ? std::string("link not connected") :
					                 !a_state     ? std::string("never written") :
					                                std::format("last write {} ms ago", a_now > a_state->time_ms ? a_now - a_state->time_ms : 0);
					spdlog::warn("[state] PlayerState stale ({}); ignoring it", why);
					last_.reset();
				}
			}
			if (!fresh) {
				return;
			}
			const auto& s = *a_state;
			if (last_) {
				if (s.stamina != last_->stamina) {
					spdlog::info("[state] stamina {}→{} er_frame={}", last_->stamina, s.stamina, s.frame);
				}
				if ((s.flags ^ last_->flags) & kIFrame) {
					spdlog::info("[state] IFrame {} er_frame={}", (s.flags & kIFrame) ? "on" : "off", s.frame);
				}
				if (s.anim_id != last_->anim_id) {
					spdlog::info("[state] anim {}→{} er_frame={}", last_->anim_id, s.anim_id, s.frame);
				}
			}
			last_ = s;
		}

		// The last fresh value (for the periodic sample line), or nullopt while stale.
		const std::optional<proto::PlayerState>& Last() const { return last_; }

	private:
		bool fresh_ = false;
		std::optional<proto::PlayerState> last_;
	};
}
