#pragma once

#include <cstdint>

// Skyrim input capture: a BSTEventSink<InputEvent*> on BSInputDeviceManager (main thread). P3 forwards the Sprint user
// event's held state as Button::Dodge and the movement keys as the move stick. F10 toggles the bridge (P4; hooks/SprintSwallow.cpp keeps
// vanilla sprint off while it's on).
namespace sxer::input
{
	// Registers the sink (on kDataLoaded). Later calls do nothing.
	void Install();
	// Bridge toggle: on at start, F10 (keyboard) flips it. Off = Skyrim plays vanilla (nothing forwarded, nothing swallowed).
	bool BridgeOn();
	// Sprint user event held right now (any device).
	bool SprintHeld();
	// Keyboard key held right now (DirectInput scan code). For debug keys.
	bool KeyHeld(std::uint32_t a_scanCode);
	// Device of the last Sprint event: "keyboard", "mouse", "gamepad", ... (for the log).
	const char* SprintDevice();
	// Movement right now: keys (Forward/Back/Strafe Left/Strafe Right user events: x = right - left, y = forward - back) plus the
	// gamepad left stick (user event Move, analog). Goes to InputState.move_x/move_y so Dodge + direction = roll in ER (P3 step 5);
	// ER picks walk or run from the length (P4 step 8).
	struct Move
	{
		float x = 0;
		float y = 0;
		bool operator==(const Move&) const = default;
	};
	Move MoveAxes();
	// ER buttons held on mouse/keyboard (InputState.buttons bits): Right Attack/Block (left mouse) = Attack, Left Attack/Block
	// (right mouse) = Guard. The pad's come from hooks/XInput (PadErButtons).
	std::uint32_t MouseErButtons();
}
