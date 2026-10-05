#pragma once

#include "bridge/PadReport.h"

#include <cstdint>
#include <optional>

// Reads a DualSense straight from Windows HID on its own thread (P4 step 8). Skyrim only reads XInput, and Steam Input doesn't apply when
// SKSE starts Skyrim outside Steam, so hooks/XInput.cpp hands this state to Skyrim as an Xbox pad.
namespace sxer::pad
{
	// Starts the reader thread (once). It finds the pad, re-scans every 2 s while none is open, and logs open/lost/button changes.
	void Start();
	// The pad's latest state; nullopt while no DualSense is open.
	std::optional<DsState> Latest();
	// Bumped on every change of the state (XINPUT_STATE.dwPacketNumber).
	std::uint32_t Packet();
}
