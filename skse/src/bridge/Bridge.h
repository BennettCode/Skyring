#pragma once

// Owns the plugin's Link and the thread that ticks it every LINK_TICK_MS.
namespace sxer::bridge
{
	// Starts the link thread (called on kDataLoaded). Later calls do nothing.
	void Start();
}
