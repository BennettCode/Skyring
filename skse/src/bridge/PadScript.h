#pragma once

#include "bridge/PadReport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Virtual pad for agent tests (P4 step 8; the idea of GTA San AnSkateas' pad-injecting test scripts, re-implemented): a script of
// pad steps, played on top of the real DualSense in hooks/XInput.cpp, so the agent can press the touchpad, open menus and switch
// tabs without the user. Only active when SKYRIMXER_PADSCRIPT names the script file (tools/launch.ps1, dev only; never for players).
// Steps, ';'-separated (same shape as tools/game-input.ps1):
//   down B | up B | tap B [ms, default 100] | wait ms | stick L|R x y (-1..1; 0 0 = centre)
// B = Cross Circle Square Triangle L1 R1 L2 R2 Create Options L3 R3 PS Touchpad Up Down Left Right (case-insensitive).
// L2/R2 pull their trigger fully while held.
namespace sxer::pad::script
{
	struct Event
	{
		std::uint32_t atMs = 0;  // from the script's start
		DsState state;           // the whole virtual pad from atMs on
	};

	inline std::optional<std::uint32_t> ButtonByName(std::string_view a_name)
	{
		constexpr std::array<std::pair<std::string_view, std::uint32_t>, 18> kNames{ { { "cross", kCross }, { "circle", kCircle },
			{ "square", kSquare }, { "triangle", kTriangle }, { "l1", kL1 }, { "r1", kR1 }, { "l2", kL2 }, { "r2", kR2 }, { "create", kCreate },
			{ "options", kOptions }, { "l3", kL3 }, { "r3", kR3 }, { "ps", kPS }, { "touchpad", kTouchpad }, { "up", kUp }, { "down", kDown },
			{ "left", kLeft }, { "right", kRight } } };
		std::string lower(a_name);
		for (auto& c : lower) {
			c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		}
		for (const auto& [name, bit] : kNames) {
			if (lower == name) {
				return bit;
			}
		}
		return std::nullopt;
	}

	// Parses a script into the pad states it goes through (first event at 0 ms, last = everything released). nullopt + a_error on a bad step.
	inline std::optional<std::vector<Event>> Parse(std::string_view a_script, std::string* a_error = nullptr)
	{
		std::vector<Event> out;
		DsState s;
		std::uint32_t t = 0;
		const auto emit = [&] {
			s.l2 = (s.buttons & kL2) ? 255 : 0;
			s.r2 = (s.buttons & kR2) ? 255 : 0;
			if (!out.empty() && out.back().atMs == t) {
				out.back().state = s;
			} else {
				out.push_back({ t, s });
			}
		};
		const auto fail = [&](std::string_view a_step) -> std::optional<std::vector<Event>> {
			if (a_error) {
				*a_error = "bad step: " + std::string(a_step);
			}
			return std::nullopt;
		};
		const auto stickValue = [](std::string_view a_v) -> std::optional<std::int16_t> {
			try {
				const float v = std::stof(std::string(a_v));
				if (v < -1.0f || v > 1.0f) {
					return std::nullopt;
				}
				return static_cast<std::int16_t>(v * 32767.0f);
			} catch (...) {
				return std::nullopt;
			}
		};
		emit();
		std::size_t pos = 0;
		while (pos <= a_script.size()) {
			const auto end = std::min(a_script.find(';', pos), a_script.size());
			std::string_view step = a_script.substr(pos, end - pos);
			pos = end + 1;
			std::vector<std::string_view> parts;
			for (std::size_t i = 0; i < step.size();) {
				while (i < step.size() && (step[i] == ' ' || step[i] == '\t' || step[i] == '\r' || step[i] == '\n')) ++i;
				std::size_t j = i;
				while (j < step.size() && step[j] != ' ' && step[j] != '\t' && step[j] != '\r' && step[j] != '\n') ++j;
				if (j > i) parts.push_back(step.substr(i, j - i));
				i = j;
			}
			if (parts.empty() || parts[0].starts_with('#')) {
				continue;
			}
			const auto verb = parts[0];
			const auto number = [&](std::size_t a_i, std::uint32_t a_default) -> std::optional<std::uint32_t> {
				if (parts.size() <= a_i) return a_default;
				try {
					return static_cast<std::uint32_t>(std::stoul(std::string(parts[a_i])));
				} catch (...) {
					return std::nullopt;
				}
			};
			if ((verb == "down" || verb == "up" || verb == "tap") && parts.size() >= 2) {
				const auto bit = ButtonByName(parts[1]);
				const auto ms = number(2, 100);
				if (!bit || !ms) return fail(step);
				if (verb == "up") {
					s.buttons &= ~*bit;
				} else {
					s.buttons |= *bit;
				}
				emit();
				if (verb == "tap") {
					t += *ms;
					s.buttons &= ~*bit;
					emit();
				}
			} else if (verb == "wait" && parts.size() == 2) {
				const auto ms = number(1, 0);
				if (!ms) return fail(step);
				t += *ms;
			} else if (verb == "stick" && parts.size() == 4 && (parts[1] == "L" || parts[1] == "R" || parts[1] == "l" || parts[1] == "r")) {
				const auto x = stickValue(parts[2]), y = stickValue(parts[3]);
				if (!x || !y) return fail(step);
				auto& sx = (parts[1] == "L" || parts[1] == "l") ? s.lx : s.rx;
				auto& sy = (parts[1] == "L" || parts[1] == "l") ? s.ly : s.ry;
				sx = *x, sy = *y;
				emit();
			} else {
				return fail(step);
			}
		}
		// The script ends with everything released.
		s = DsState{};
		emit();
		return out;
	}

	// The virtual pad a_ms after the start (the last event at or before it).
	inline DsState At(const std::vector<Event>& a_events, std::uint32_t a_ms)
	{
		DsState s;
		for (const auto& e : a_events) {
			if (e.atMs > a_ms) break;
			s = e.state;
		}
		return s;
	}

	// Total length (time of the last event).
	inline std::uint32_t LengthMs(const std::vector<Event>& a_events) { return a_events.empty() ? 0 : a_events.back().atMs; }

	// Runtime (PadScript.cpp): watches the SKYRIMXER_PADSCRIPT file and plays each new version once.
	void Start();
	// The virtual pad right now; nullopt when no script is playing.
	std::optional<DsState> Overlay();
}
