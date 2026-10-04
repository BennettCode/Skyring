#pragma once

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
	// Device of the last Sprint event: "keyboard", "mouse", "gamepad", ... (for the log).
	const char* SprintDevice();
	// Movement keys held right now (Forward/Back/Strafe Left/Strafe Right user events, keyboard): x = right - left,
	// y = forward - back, each -1, 0 or 1. Goes to InputState.move_x/move_y so Dodge + direction = roll in ER (P3 step 5).
	struct Move
	{
		float x = 0;
		float y = 0;
		bool operator==(const Move&) const = default;
	};
	Move MoveAxes();
}
