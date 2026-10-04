#pragma once

// Skyrim input capture: a BSTEventSink<InputEvent*> on BSInputDeviceManager (main thread). P3 forwards the Sprint user
// event's held state as Button::Dodge and the movement keys as the move stick; vanilla sprint still fires until P4 swallows input.
namespace sxer::input
{
	// Registers the sink (on kDataLoaded). Later calls do nothing.
	void Install();
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
