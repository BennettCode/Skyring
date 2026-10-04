#pragma once

// Skyrim input capture: a BSTEventSink<InputEvent*> on BSInputDeviceManager (main thread). P3 forwards the Sprint user
// event's held state as Button::Dodge; vanilla sprint still fires until P4 swallows input.
namespace sxer::input
{
	// Registers the sink (on kDataLoaded). Later calls do nothing.
	void Install();
	// Sprint user event held right now (any device).
	bool SprintHeld();
	// Device of the last Sprint event: "keyboard", "mouse", "gamepad", ... (for the log).
	const char* SprintDevice();
}
