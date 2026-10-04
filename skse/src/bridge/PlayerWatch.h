#pragma once

// Logs PlayerState edges on the Skyrim side: fresh/stale transitions, stamina drops (regen squashed into one line when it reaches
// max or is interrupted), IFrame on/off, anim changes.
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

		void Update(std::optional<proto::PlayerState> a_state, bool a_connected, std::uint64_t a_now)
		{
			// A failed read (every try torn: the writer was pre-empted mid-write) is not staleness: judge the last good copy by its age.
			if (!a_state) {
				a_state = last_;
			}
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
					regenFrom_.reset();
				}
			}
			if (!fresh) {
				return;
			}
			const auto& s = *a_state;
			if (last_) {
				if (s.stamina < last_->stamina) {
					if (regenFrom_) {
						spdlog::info("[state] stamina regen {}→{} (interrupted)", *regenFrom_, last_->stamina);
						regenFrom_.reset();
					}
					spdlog::info("[state] stamina {}→{} er_frame={}", last_->stamina, s.stamina, s.frame);
				} else if (s.stamina > last_->stamina) {
					if (!regenFrom_) {
						regenFrom_ = last_->stamina;
					}
					if (s.stamina >= s.max_stamina) {
						spdlog::info("[state] stamina regen {}→{} er_frame={}", *regenFrom_, s.stamina, s.frame);
						regenFrom_.reset();
					}
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
		std::optional<std::int32_t> regenFrom_;  // stamina where the current regen started
	};
}
