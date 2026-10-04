#pragma once

// Coordinate conversion between the two games: C++ mirror of protocol/src/coords.rs (same names, same tests in
// skse/tests/link_test.cpp). Only deltas in the character's own frame cross over (docs/research/coordinates.md, DESIGN §6).
//
// Measured conventions (both games, 2026-10-04):
// - Skyrim: Z up, 70 units ≈ 1 m. Heading = data.angle.z; forward = (sin h, cos h, 0), right = (cos h, -sin h, 0).
// - Elden Ring: Y up, 1 unit = 1 m. Yaw about Y; forward = (-sin y, 0, -cos y), right = (-cos y, 0, sin y).
// - In both, a growing angle = turning right (clockwise seen from above).

#include <array>
#include <cmath>

namespace sxer::coords
{
	// Skyrim units per metre (1 unit = 1.428 cm; documented, not measured).
	inline constexpr float kSkyrimUnitsPerM = 70.0f;

	// A delta in the character's own frame, metres.
	struct Local
	{
		float forward = 0;
		float right = 0;
		float up = 0;
	};

	using Vec3 = std::array<float, 3>;

	// ER world delta {x, y, z} (metres, Y up) -> character frame, for a character facing a_yaw.
	inline Local ErDeltaToLocal(const Vec3& a_d, float a_yaw)
	{
		const float s = std::sin(a_yaw), c = std::cos(a_yaw);
		return { -a_d[0] * s - a_d[2] * c, -a_d[0] * c + a_d[2] * s, a_d[1] };
	}

	// Character frame -> ER world delta {x, y, z} (metres, Y up), for a character facing a_yaw.
	inline Vec3 LocalToErDelta(const Local& a_l, float a_yaw)
	{
		const float s = std::sin(a_yaw), c = std::cos(a_yaw);
		return { -a_l.forward * s - a_l.right * c, a_l.up, -a_l.forward * c + a_l.right * s };
	}

	// Skyrim world delta {x, y, z} (game units, Z up) -> character frame (metres), for a character with a_heading (GetAngleZ()).
	inline Local SkyrimDeltaToLocal(const Vec3& a_d, float a_heading)
	{
		const float s = std::sin(a_heading), c = std::cos(a_heading);
		return { (a_d[0] * s + a_d[1] * c) / kSkyrimUnitsPerM, (a_d[0] * c - a_d[1] * s) / kSkyrimUnitsPerM, a_d[2] / kSkyrimUnitsPerM };
	}

	// Character frame (metres) -> Skyrim world delta {x, y, z} (game units, Z up), for a character with a_heading.
	inline Vec3 LocalToSkyrimDelta(const Local& a_l, float a_heading)
	{
		const float s = std::sin(a_heading), c = std::cos(a_heading);
		return { (a_l.forward * s + a_l.right * c) * kSkyrimUnitsPerM, (a_l.forward * c - a_l.right * s) * kSkyrimUnitsPerM,
			a_l.up * kSkyrimUnitsPerM };
	}

	// An ER yaw change (radians) -> the Skyrim heading change that turns the same way (both grow when turning right).
	inline float ErYawDeltaToSkyrim(float a_d) { return a_d; }
}
