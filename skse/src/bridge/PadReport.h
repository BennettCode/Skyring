#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>

// DualSense HID input report → pad state, and the button layouts (P4 step 8). Pure code, no Windows or game headers, so the selftest
// can check it. Report layouts (public DualSense HID documentation, e.g. the Linux hid-playstation driver):
//   USB 0x01, 64 bytes: [1..4] LX LY RX RY, [5] L2, [6] R2, [7] counter, [8] hat (low nibble) + Square/Cross/Circle/Triangle (bits 4-7),
//                       [9] L1 R1 L2 R2 Create Options L3 R3 (bits 0-7), [10] PS, touchpad, mute.
//   Bluetooth 0x31 (extended): the same fields one byte later.
//   Bluetooth 0x01 (simple, before the extended mode is switched on): [1..4] sticks, [5] hat + face, [6] L1..R3, [7] PS + touchpad, [8] L2, [9] R2.
namespace sxer::pad
{
	// Same layout as XINPUT_GAMEPAD.
	struct PadState
	{
		std::uint16_t buttons = 0;
		std::uint8_t leftTrigger = 0;
		std::uint8_t rightTrigger = 0;
		std::int16_t thumbLX = 0;
		std::int16_t thumbLY = 0;
		std::int16_t thumbRX = 0;
		std::int16_t thumbRY = 0;
		bool operator==(const PadState&) const = default;
	};
	static_assert(sizeof(PadState) == 12);

	// XINPUT_GAMEPAD button bits.
	enum Button : std::uint16_t
	{
		kDpadUp = 0x0001,
		kDpadDown = 0x0002,
		kDpadLeft = 0x0004,
		kDpadRight = 0x0008,
		kStart = 0x0010,
		kBack = 0x0020,
		kLeftThumb = 0x0040,
		kRightThumb = 0x0080,
		kLeftShoulder = 0x0100,
		kRightShoulder = 0x0200,
		kA = 0x1000,
		kB = 0x2000,
		kX = 0x4000,
		kY = 0x8000,
	};

	// DualSense buttons by name (an Xbox pad is translated to the same positions: A = Cross, B = Circle, ...).
	enum Ds : std::uint32_t
	{
		kCross = 1u << 0,
		kCircle = 1u << 1,
		kSquare = 1u << 2,
		kTriangle = 1u << 3,
		kL1 = 1u << 4,
		kR1 = 1u << 5,
		kL2 = 1u << 6,
		kR2 = 1u << 7,
		kCreate = 1u << 8,
		kOptions = 1u << 9,
		kL3 = 1u << 10,
		kR3 = 1u << 11,
		kPS = 1u << 12,
		kTouchpad = 1u << 13,
		kUp = 1u << 14,
		kDown = 1u << 15,
		kLeft = 1u << 16,
		kRight = 1u << 17,
	};

	struct DsState
	{
		std::uint32_t buttons = 0;
		std::uint8_t l2 = 0;
		std::uint8_t r2 = 0;
		std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // XInput scale and direction (Y up = +)
		bool operator==(const DsState&) const = default;
	};

	// ER buttons a pad press asks for, as InputState.buttons bits (protocol Button: Attack = 1, StrongAttack = 2, Guard = 3, Skill = 4;
	// XInput.cpp checks these against the generated enum). Dodge (bit 0) keeps coming from Skyrim's Sprint user event.
	enum ErButton : std::uint32_t
	{
		kErAttack = 1u << 1,
		kErStrongAttack = 1u << 2,
		kErGuard = 1u << 3,
		kErSkill = 1u << 4,
	};

	// 0..255 (128 = centre) → -32768..32767.
	constexpr std::int16_t Axis(std::uint8_t a_v) { return static_cast<std::int16_t>(a_v * 257 - 32768); }
	// Same, flipped (the DualSense's Y grows downward, XInput's upward).
	constexpr std::int16_t AxisFlipped(std::uint8_t a_v) { return static_cast<std::int16_t>(-(a_v * 257 - 32768) - 1); }

	// Parses one input report (first byte = report id). Windows pads every read to the device's input report length, so the
	// connection decides the layout, not the size. nullopt for anything else.
	inline std::optional<DsState> ParseReport(const std::uint8_t* a_data, std::size_t a_size, bool a_bluetooth)
	{
		if (!a_data || a_size < 10) {
			return std::nullopt;
		}
		std::size_t sticks = 1, face = 0, shoulders = 0, ps = 0, l2 = 0, r2 = 0;
		if (!a_bluetooth && a_data[0] == 0x01 && a_size >= 11) {
			face = 8, shoulders = 9, ps = 10, l2 = 5, r2 = 6;  // USB
		} else if (a_bluetooth && a_data[0] == 0x31 && a_size >= 12) {
			sticks = 2, face = 9, shoulders = 10, ps = 11, l2 = 6, r2 = 7;  // Bluetooth extended
		} else if (a_bluetooth && a_data[0] == 0x01) {
			face = 5, shoulders = 6, ps = 7, l2 = 8, r2 = 9;  // Bluetooth simple
		} else {
			return std::nullopt;
		}
		DsState s;
		s.lx = Axis(a_data[sticks]);
		s.ly = AxisFlipped(a_data[sticks + 1]);
		s.rx = Axis(a_data[sticks + 2]);
		s.ry = AxisFlipped(a_data[sticks + 3]);
		s.l2 = a_data[l2];
		s.r2 = a_data[r2];

		const std::uint8_t f = a_data[face];
		// Hat: 0 = up, then clockwise in 45° steps, 8+ = released.
		constexpr std::uint32_t kHat[8] = { kUp, kUp | kRight, kRight, kDown | kRight, kDown, kDown | kLeft, kLeft, kUp | kLeft };
		const auto hat = f & 0x0F;
		std::uint32_t b = hat < 8 ? kHat[hat] : 0;
		if (f & 0x10) b |= kSquare;
		if (f & 0x20) b |= kCross;
		if (f & 0x40) b |= kCircle;
		if (f & 0x80) b |= kTriangle;
		const std::uint8_t sh = a_data[shoulders];
		constexpr std::uint32_t kShoulder[8] = { kL1, kR1, kL2, kR2, kCreate, kOptions, kL3, kR3 };
		for (int i = 0; i < 8; ++i) {
			if (sh & (1 << i)) b |= kShoulder[i];
		}
		if (a_data[ps] & 0x01) b |= kPS;
		if (a_data[ps] & 0x02) b |= kTouchpad;
		s.buttons = b;
		return s;
	}

	// An Xbox pad (or Steam Input's virtual one) in DualSense terms. Triggers count as pressed from 30/255 (XInput's own threshold).
	constexpr DsState FromXInput(const PadState& a_x)
	{
		constexpr std::uint16_t kFrom[] = { kA, kB, kX, kY, kLeftShoulder, kRightShoulder, kBack, kStart, kLeftThumb, kRightThumb, kDpadUp,
			kDpadDown, kDpadLeft, kDpadRight };
		constexpr std::uint32_t kTo[] = { kCross, kCircle, kSquare, kTriangle, kL1, kR1, kCreate, kOptions, kL3, kR3, kUp, kDown, kLeft, kRight };
		DsState s;
		for (std::size_t i = 0; i < std::size(kFrom); ++i) {
			if (a_x.buttons & kFrom[i]) s.buttons |= kTo[i];
		}
		if (a_x.leftTrigger > 30) s.buttons |= kL2;
		if (a_x.rightTrigger > 30) s.buttons |= kR2;
		s.l2 = a_x.leftTrigger, s.r2 = a_x.rightTrigger;
		s.lx = a_x.thumbLX, s.ly = a_x.thumbLY, s.rx = a_x.thumbRX, s.ry = a_x.thumbRY;
		return s;
	}

	namespace detail
	{
		struct Route
		{
			std::uint32_t ds;
			std::uint16_t xinput;
		};

		constexpr PadState Map(const DsState& a_s, const Route* a_routes, std::size_t a_count, bool a_triggers)
		{
			PadState x;
			for (std::size_t i = 0; i < a_count; ++i) {
				if (a_s.buttons & a_routes[i].ds) x.buttons |= a_routes[i].xinput;
			}
			x.leftTrigger = a_triggers ? a_s.l2 : 0;
			x.rightTrigger = a_triggers ? a_s.r2 : 0;
			x.thumbLX = a_s.lx, x.thumbLY = a_s.ly, x.thumbRX = a_s.rx, x.thumbRY = a_s.ry;
			return x;
		}

		// Menus, bridge off: plain Xbox positions (Circle = back). The touchpad does nothing here (in gameplay it opens the map; held
		// into the map it used to send Back = Wait).
		constexpr Route kMenu[] = { { kCross, kA }, { kCircle, kB }, { kSquare, kX }, { kTriangle, kY }, { kL1, kLeftShoulder },
			{ kR1, kRightShoulder }, { kCreate, kBack }, { kOptions, kStart }, { kL3, kLeftThumb }, { kR3, kRightThumb }, { kUp, kDpadUp },
			{ kDown, kDpadDown }, { kLeft, kDpadLeft }, { kRight, kDpadRight } };

		// Gameplay with the bridge on (user layout 2026-10-05: Elden Ring's buttons, Skyrim's extras where ER has the nearest job).
		// Skyrim's default gamepad events (logged): A Activate, B Tween Menu, X Ready Weapon, Y Jump, LB Sprint, RB Shout, L3 Sneak,
		// R3 Toggle POV, Start Journal, d-pad up Favorites. R1/R2/L1/L2 are ER's only and Skyrim never sees them. The touchpad opens
		// the map like in ER (no Skyrim pad button does: hooks/XInput.cpp asks for the Map menu), so the Journal (Skyrim's pause menu)
		// sits on Create. Skyrim's Wait (Back) has no pad button in this layout (keyboard T still works).
		constexpr Route kGameplay[] = { { kCross, kY }, { kTriangle, kA }, { kSquare, kX }, { kCircle, kLeftShoulder }, { kOptions, kB },
			{ kCreate, kStart }, { kL3, kLeftThumb }, { kR3, kRightThumb }, { kUp, kRightShoulder }, { kDown, kDpadUp }, { kLeft, kDpadLeft },
			{ kRight, kDpadRight } };
	}

	// a_shoulderTabs: the Journal switches tabs with LT/RT (controlmap "TabSwitch"); ER players reach for L1/R1, so they press the
	// triggers too while it's open.
	constexpr PadState ToXInputMenu(const DsState& a_s, bool a_shoulderTabs = false)
	{
		auto x = detail::Map(a_s, detail::kMenu, std::size(detail::kMenu), true);
		// Menus use the triggers as buttons (Journal tabs, equip left/right): all or nothing from XInput's threshold (30), so a slow
		// half press arrives as one clean press instead of a creeping analog value.
		x.leftTrigger = a_s.l2 > 30 ? 255 : 0;
		x.rightTrigger = a_s.r2 > 30 ? 255 : 0;
		if (a_shoulderTabs) {
			if (a_s.buttons & kL1) x.leftTrigger = 255;
			if (a_s.buttons & kR1) x.rightTrigger = 255;
		}
		return x;
	}

	constexpr PadState ToXInputGameplay(const DsState& a_s)
	{
		return detail::Map(a_s, detail::kGameplay, std::size(detail::kGameplay), false);
	}

	constexpr std::uint32_t ToErButtons(const DsState& a_s)
	{
		std::uint32_t er = 0;
		if (a_s.buttons & kR1) er |= kErAttack;
		if (a_s.buttons & kR2) er |= kErStrongAttack;
		if (a_s.buttons & kL1) er |= kErGuard;
		if (a_s.buttons & kL2) er |= kErSkill;
		return er;
	}
}
