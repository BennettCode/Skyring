#pragma once

// Pose streaming, Skyrim side (docs/POSE-PLAN.md). Step 2 = one-bone proof, no ER involved:
// - once the player's third-person 3D exists: logs its bone tree and the body's skin bones (bind pose source) as [pose] lines;
// - F7 held: pelvis turned 45° right after PlayerCharacter::Update (vfunc 0xAD); the first frames trace whether the write is still
//   there at the next Update (the animation re-poses the skeleton in between, on a worker thread).
// Measured 2026-10-04: a write here shows on screen (the legs turn). UpdateAnimation (vfunc 0x7D) runs on another thread, before the
// next Update; writes there would race the main thread, so we don't use it. Main thread only.
namespace sxer::pose
{
	void BeforePlayerUpdate(RE::PlayerCharacter* a_player);
	void AfterPlayerUpdate(RE::PlayerCharacter* a_player);
}
