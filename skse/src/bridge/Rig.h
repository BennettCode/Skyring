#pragma once

// Pose retarget math: C++ mirror of protocol/src/rig.rs (same names in this file's style, same tests in skse/tests/link_test.cpp).
// - Quaternions are {x, y, z, w}, Hamilton product, rotating column vectors (v' = q v q*), as Havok and NetImmerse do.
// - A bone's delta is q_model * q_bind_model^-1 (rig.rs); the ER side already converts it to Skyrim's model basis, so no Basis here.
// - Slerp and RotationArc are only needed on this side (blending, and aligning Skyrim's bind segments to ER's).
// RE-free on purpose: skyrimxer_link_test tests it without the game.

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace sxer::rig
{
	struct Quat
	{
		float x = 0, y = 0, z = 0, w = 1;
	};
	using Vec3 = std::array<float, 3>;
	using Mat3 = std::array<std::array<float, 3>, 3>;  // m[row][col], v' = m * v (column vectors)

	inline constexpr Quat kIdentity{};

	inline Quat Mul(Quat a_a, Quat a_b)
	{
		return { a_a.w * a_b.x + a_a.x * a_b.w + a_a.y * a_b.z - a_a.z * a_b.y, a_a.w * a_b.y - a_a.x * a_b.z + a_a.y * a_b.w + a_a.z * a_b.x,
			a_a.w * a_b.z + a_a.x * a_b.y - a_a.y * a_b.x + a_a.z * a_b.w, a_a.w * a_b.w - a_a.x * a_b.x - a_a.y * a_b.y - a_a.z * a_b.z };
	}

	inline Quat Conj(Quat a_q) { return { -a_q.x, -a_q.y, -a_q.z, a_q.w }; }

	inline float Norm(Quat a_q) { return std::sqrt(a_q.x * a_q.x + a_q.y * a_q.y + a_q.z * a_q.z + a_q.w * a_q.w); }

	// Unit length, w >= 0; nullopt for a zero or non-finite input.
	inline std::optional<Quat> Normalize(Quat a_q)
	{
		const float n = Norm(a_q);
		if (!std::isfinite(n) || n < 1e-6f) {
			return std::nullopt;
		}
		const float s = a_q.w < 0 ? -1.0f / n : 1.0f / n;
		return Quat{ a_q.x * s, a_q.y * s, a_q.z * s, a_q.w * s };
	}

	inline Quat AxisAngle(Vec3 a_axis, float a_angle)
	{
		const float s = std::sin(a_angle * 0.5f), c = std::cos(a_angle * 0.5f);
		return { a_axis[0] * s, a_axis[1] * s, a_axis[2] * s, c };
	}

	// Rotation angle of a_q in radians (0..pi).
	inline float Angle(Quat a_q) { return 2.0f * std::acos(std::min(std::fabs(a_q.w), 1.0f)); }

	inline Vec3 Rotate(Quat a_q, Vec3 a_v)
	{
		const auto p = Mul(Mul(a_q, Quat{ a_v[0], a_v[1], a_v[2], 0 }), Conj(a_q));
		return { p.x, p.y, p.z };
	}

	// Rotation matrix of a unit quaternion.
	inline Mat3 ToMat(Quat a_q)
	{
		const float x = a_q.x, y = a_q.y, z = a_q.z, w = a_q.w;
		return { { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
			{ 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
			{ 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } } };
	}

	// Unit quaternion (w >= 0) of a rotation matrix (Shepperd: branch on the largest diagonal term).
	inline Quat FromMat(const Mat3& a_m)
	{
		const auto& m = a_m;
		const float t = m[0][0] + m[1][1] + m[2][2];
		Quat q;
		if (t > 0) {
			const float s = std::sqrt(t + 1) * 2;
			q = { (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25f * s };
		} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
			const float s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
			q = { 0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s };
		} else if (m[1][1] > m[2][2]) {
			const float s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
			q = { (m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s };
		} else {
			const float s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
			q = { (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s, (m[1][0] - m[0][1]) / s };
		}
		return Normalize(q).value_or(kIdentity);
	}

	// q_model * q_bind_model^-1, unit, w >= 0.
	inline Quat Delta(Quat a_model, Quat a_bind) { return Normalize(Mul(a_model, Conj(a_bind))).value_or(kIdentity); }

	inline float Dot(Vec3 a_a, Vec3 a_b) { return a_a[0] * a_b[0] + a_a[1] * a_b[1] + a_a[2] * a_b[2]; }

	inline Vec3 Sub(Vec3 a_a, Vec3 a_b) { return { a_a[0] - a_b[0], a_a[1] - a_b[1], a_a[2] - a_b[2] }; }

	inline std::optional<Vec3> Unit(Vec3 a_a)
	{
		const float n = std::sqrt(Dot(a_a, a_a));
		if (!std::isfinite(n) || n < 1e-6f) {
			return std::nullopt;
		}
		return Vec3{ a_a[0] / n, a_a[1] / n, a_a[2] / n };
	}

	// Shortest-path spherical interpolation (a_t = 0 → a_a, 1 → a_b); normalized lerp when the two are nearly equal.
	inline Quat Slerp(Quat a_a, Quat a_b, float a_t)
	{
		float d = a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z + a_a.w * a_b.w;
		if (d < 0) {
			a_b = { -a_b.x, -a_b.y, -a_b.z, -a_b.w };
			d = -d;
		}
		float wa = 1 - a_t, wb = a_t;
		if (d < 0.9995f) {
			const float theta = std::acos(std::min(d, 1.0f));
			const float s = std::sin(theta);
			wa = std::sin((1 - a_t) * theta) / s;
			wb = std::sin(a_t * theta) / s;
		}
		const Quat q{ wa * a_a.x + wb * a_b.x, wa * a_a.y + wb * a_b.y, wa * a_a.z + wb * a_b.z, wa * a_a.w + wb * a_b.w };
		return Normalize(q).value_or(a_a);
	}

	// Minimal rotation taking unit a_from onto unit a_to. Antiparallel: 180 degrees about any axis perpendicular to a_from.
	inline Quat RotationArc(Vec3 a_from, Vec3 a_to)
	{
		const float d = Dot(a_from, a_to);
		if (d < -0.999999f) {
			auto axis = Unit({ 0, -a_from[2], a_from[1] });  // a_from × X
			if (!axis) {
				axis = Unit({ a_from[2], 0, -a_from[0] });  // a_from × Y
			}
			return AxisAngle(axis.value_or(Vec3{ 0, 0, 1 }), 3.14159265f);
		}
		const Vec3 c{ a_from[1] * a_to[2] - a_from[2] * a_to[1], a_from[2] * a_to[0] - a_from[0] * a_to[2], a_from[0] * a_to[1] - a_from[1] * a_to[0] };
		return Normalize(Quat{ c[0], c[1], c[2], 1 + d }).value_or(kIdentity);
	}
}
