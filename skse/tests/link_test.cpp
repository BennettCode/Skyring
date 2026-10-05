// skyrimxer_link_test: runs the plugin's C++ Link (skse/src/bridge/Link.cpp) outside Skyrim.
//
//   skyrimxer_link_test selftest
//       Two Links (Skyrim + ER) in this process on a private region, simulated clock: handshake, heartbeat events,
//       timeout, reconnect, Bye, LinkShared, plus seqlock slots (round trip, concurrent torn-read check) and the coordinate
//       conversion (bridge/Coords.h, same cases as protocol/src/coords.rs) and the pose retarget math (bridge/Rig.h, same
//       cases as protocol/src/rig.rs, plus Slerp and RotationArc).
//       Exit 0 = pass. Run by ctest and tests/run-tests.ps1.
//   skyrimxer_link_test peer [--side skyrim|er] [--seconds N] [--no-bye] [--region NAME] [--dodge-every S]
//       Real-time peer, for cross-language tests against `cargo run -p fake-peer`. Same thread shape as the plugin: the link
//       ticks on its own thread, the main thread runs ~60 "frames"/s and touches only the slots. As skyrim it holds Dodge
//       for 250 ms (first 1 s after connecting, then every S seconds, default 4) and logs PlayerState edges with the
//       same wording as tools/fake-peer (bridge/PlayerWatch.h), plus PoseState Active edges (PoseWatch).
//
// The generated header's static_asserts also make this target the C++ layout test.

#include "bridge/Coords.h"
#include "bridge/Link.h"
#include "bridge/PadReport.h"
#include "bridge/PadScript.h"
#include "bridge/PlayerWatch.h"
#include "bridge/Rig.h"
#include "bridge/Slot.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <Windows.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

namespace
{
	using sxer::Link;
	using sxer::PeerStatus;
	using sxer::Side;
	using sxer::proto::InputState;
	using sxer::proto::PlayerState;
	using sxer::proto::PoseState;

	constexpr std::uint32_t kDodge = 1u << static_cast<std::uint32_t>(sxer::proto::Button::Dodge);
	constexpr std::uint32_t kPoseActive = 1u << static_cast<std::uint32_t>(sxer::proto::PoseFlag::Active);
	static_assert(std::size(PoseState{}.rot) == 4 * sxer::proto::kPoseBoneCount, "rot = 4 floats per PoseBone");

	// Logs PoseState Active edges (peer mode): bone count on, and on off the frames seen, the largest pelvis swing and how
	// many quaternions were not unit length. tests/run-tests.ps1 matches this wording.
	class PoseWatch
	{
	public:
		void Update(std::optional<PoseState> a_pose, bool a_connected, std::uint64_t a_now)
		{
			if (a_pose) {
				last_ = a_pose;
			}
			const bool active = a_connected && last_ && sxer::Fresh(last_->time_ms, a_now) && (last_->flags & kPoseActive);
			if (active && !active_) {
				spdlog::info("[pose] Active on bones={} er_frame={}", last_->bone_count, last_->frame);
				frames_ = 0, bad_ = 0, maxDeg_ = 0;
			} else if (!active && active_) {
				spdlog::info("[pose] Active off er_frame={} (frames={} pelvis swing max={:.0f} deg, bad quats={})", last_ ? last_->frame : 0,
					frames_, maxDeg_, bad_);
			}
			active_ = active;
			if (!active || !a_pose) {
				return;
			}
			++frames_;
			const auto& r = a_pose->rot;
			for (std::size_t i = 0; i < std::size(r); i += 4) {
				const float n = r[i] * r[i] + r[i + 1] * r[i + 1] + r[i + 2] * r[i + 2] + r[i + 3] * r[i + 3];
				bad_ += std::fabs(n - 1.0f) > 1e-3f ? 1 : 0;
			}
			const auto p = static_cast<std::size_t>(sxer::proto::PoseBone::Pelvis) * 4;
			maxDeg_ = std::max(maxDeg_, 2.0f * std::acos(std::clamp(std::fabs(r[p + 3]), 0.0f, 1.0f)) * 57.29578f);
		}

	private:
		std::optional<PoseState> last_;
		bool active_ = false;
		std::uint64_t frames_ = 0, bad_ = 0;
		float maxDeg_ = 0;
	};

	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		std::printf("  %s %s\n", a_ok ? "ok  " : "FAIL", a_what);
		g_failures += a_ok ? 0 : 1;
	}

	// Every field derives from one counter, so a torn copy shows up as fields that disagree.
	PlayerState Snapshot(std::uint64_t a_k)
	{
		const auto i = static_cast<std::int32_t>(a_k);
		const auto f = static_cast<float>(a_k % 100000);
		PlayerState s{};
		s.flags = static_cast<std::uint32_t>(a_k);
		s.frame = a_k;
		s.time_ms = a_k * 3;
		s.hp = i;
		s.max_hp = i + 1;
		s.fp = i + 2;
		s.max_fp = i + 3;
		s.stamina = i + 4;
		s.max_stamina = i + 5;
		s.anim_id = i + 6;
		s.block_id = i + 7;
		s.poise = f;
		s.poise_max = f + 1;
		s.pos[0] = f + 2;
		s.pos[1] = f + 3;
		s.pos[2] = f + 4;
		s.yaw = f + 5;
		return s;
	}

	bool SameExceptSeq(PlayerState a_a, PlayerState a_b)
	{
		a_a.seq = a_b.seq = 0;
		return std::memcmp(&a_a, &a_b, sizeof(PlayerState)) == 0;
	}

	bool Near(float a_a, float a_b, float a_tol) { return std::fabs(a_a - a_b) <= a_tol; }

	// Same cases as protocol/src/coords.rs tests, including the real segments from the 2026-10-04 both-games walk.
	void CoordsTests()
	{
		using namespace sxer::coords;
		constexpr float kHalfPi = 1.57079633f;
		const Local l{ 1.5f, -0.7f, 0.3f };
		bool roundTrip = true;
		for (const float angle : { -3.0f, -1.2f, 0.0f, 0.4f, 1.7382f, 3.1f }) {
			for (const auto got : { ErDeltaToLocal(LocalToErDelta(l, angle), angle), SkyrimDeltaToLocal(LocalToSkyrimDelta(l, angle), angle) }) {
				roundTrip &= Near(got.forward, l.forward, 1e-5f) && Near(got.right, l.right, 1e-5f) && Near(got.up, l.up, 1e-5f);
			}
		}
		Check(roundTrip, "coords: round trips");
		const auto skyFwd0 = LocalToSkyrimDelta({ 1, 0, 0 }, 0), skyFwd90 = LocalToSkyrimDelta({ 1, 0, 0 }, kHalfPi);
		Check(Near(skyFwd0[0], 0, 1e-4f) && Near(skyFwd0[1], 70, 1e-4f) && Near(skyFwd90[0], 70, 1e-4f) && Near(skyFwd90[1], 0, 1e-3f),
			"coords: Skyrim heading 0 = +Y, +90 deg = +X");
		const auto erFwd0 = LocalToErDelta({ 1, 0, 2 }, 0), erFwd90 = LocalToErDelta({ 1, 0, 0 }, kHalfPi);
		Check(Near(erFwd0[0], 0, 1e-6f) && Near(erFwd0[1], 2, 1e-6f) && Near(erFwd0[2], -1, 1e-6f) && Near(erFwd90[0], -1, 1e-6f) &&
				  Near(erFwd90[2], 0, 1e-6f),
			"coords: ER yaw 0 = -Z, +90 deg = -X, Y up");
		const auto skyW = SkyrimDeltaToLocal({ 173884.2f - 173080.8f, -91225.6f - -91143.9f, 11095.0f - 11110.9f }, 1.6274f);
		const auto skyD = SkyrimDeltaToLocal({ 173964.4f - 174001.6f, -91867.7f - -91232.6f, 11154.1f - 11083.6f }, 1.6274f);
		Check(Near(skyW.forward, 11.52f, 0.05f) && std::fabs(skyW.right) < 0.06f * skyW.forward && Near(skyD.right, 9.09f, 0.05f) &&
				  std::fabs(skyD.forward) < 0.05f,
			"coords: measured Skyrim W/D segments");
		const auto erW = ErDeltaToLocal({ 4.347f - 5.510f, 6.756f - 6.634f, 5.472f - 5.275f }, 1.7382f);
		const auto erD = ErDeltaToLocal({ -4.100f - -4.284f, 8.810f - 8.751f, 8.226f - 7.462f }, 1.7382f);
		Check(Near(erW.forward, 1.18f, 0.01f) && std::fabs(erW.right) < 0.01f && Near(erD.right, 0.78f, 0.01f) && std::fabs(erD.forward) < 0.1f,
			"coords: measured ER W/D segments");
	}

	// Same cases as protocol/src/rig.rs, plus Slerp and RotationArc (C++ only).
	void RigTests()
	{
		using namespace sxer::rig;
		const auto nearV = [](Vec3 a_a, Vec3 a_b) { return Near(a_a[0], a_b[0], 1e-4f) && Near(a_a[1], a_b[1], 1e-4f) && Near(a_a[2], a_b[2], 1e-4f); };
		// Same rotation (q and -q are equal rotations).
		const auto same = [](Quat a_a, Quat a_b) { return std::fabs(a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z + a_a.w * a_b.w) > 1 - 1e-5f; };
		const auto mulV = [](const Mat3& a_m, Vec3 a_v) { return Vec3{ Dot(a_m[0], a_v), Dot(a_m[1], a_v), Dot(a_m[2], a_v) }; };

		bool roundTrip = true, rotateMatches = true;
		for (auto q : { kIdentity, AxisAngle({ 0, 0, 1 }, 3.14159265f), AxisAngle({ 0.6f, 0, 0.8f }, 2.0f), Quat{ 0.70f, 0.70f, -0.05f, -0.12f } }) {
			q = *Normalize(q);
			roundTrip &= same(FromMat(ToMat(q)), q);
			rotateMatches &= nearV(Rotate(q, { 1, 2, 3 }), mulV(ToMat(q), { 1, 2, 3 }));
		}
		Check(roundTrip, "rig: quaternion -> matrix -> quaternion round trip");
		Check(rotateMatches, "rig: Rotate == ToMat * v");

		const auto bind = *Normalize({ 0.707f, 0.707f, 0, 0 });
		Check(same(Delta(bind, bind), kIdentity), "rig: identity bind gives identity delta");
		const auto turn = AxisAngle({ 0, 1, 0 }, 0.5f);
		Check(same(Delta(Mul(turn, bind), bind), turn), "rig: delta recovers the model-space turn");

		const auto a = AxisAngle({ 0, 0, 1 }, 0.3f);
		const auto b = AxisAngle({ 0, 0, 1 }, 0.3f + 1.5707963f);
		Check(same(Slerp(a, b, 0), a) && same(Slerp(a, b, 1), b), "rig: Slerp endpoints");
		Check(Near(Angle(Delta(Slerp(a, b, 0.5f), a)), 0.7853982f, 1e-4f), "rig: Slerp halfway through a 90 deg turn = 45 deg");
		const Quat negB{ -b.x, -b.y, -b.z, -b.w };
		Check(same(Slerp(a, negB, 0.5f), Slerp(a, b, 0.5f)), "rig: Slerp takes the short path (b and -b give the same result)");

		const Vec3 from = *Unit({ 0.3f, -0.5f, 0.8f }), to = *Unit({ -0.7f, 0.1f, 0.2f });
		Check(nearV(Rotate(RotationArc(from, to), from), to), "rig: RotationArc maps from onto to");
		Check(same(RotationArc(to, to), kIdentity), "rig: RotationArc of parallel vectors = identity");
		const Vec3 back{ -to[0], -to[1], -to[2] };
		const auto flip = RotationArc(to, back);
		Check(nearV(Rotate(flip, to), back) && Near(Angle(flip), 3.14159265f, 1e-3f), "rig: RotationArc of antiparallel vectors = 180 deg turn");
		const Vec3 x{ 1, 0, 0 }, negX{ -1, 0, 0 };
		Check(nearV(Rotate(RotationArc(x, negX), x), negX), "rig: RotationArc antiparallel along an axis");
	}

	// DualSense report parser and button layouts (bridge/PadReport.h): USB, Bluetooth extended and simple; menu/gameplay/ER mappings.
	void PadTests()
	{
		using namespace sxer::pad;
		std::uint8_t usb[64]{ 0x01, 128, 128, 128, 128, 0, 0, 0, 0x08 };
		auto s = ParseReport(usb, sizeof(usb), false);
		Check(s && s->buttons == 0 && s->lx == 128 && s->ly == -129 && s->l2 == 0, "pad: USB neutral");
		usb[1] = 0, usb[2] = 0, usb[3] = 255, usb[4] = 255, usb[5] = 200, usb[6] = 255;
		usb[8] = 0x40 | 0x20 | 0x02, usb[9] = 0x01 | 0x20 | 0x80, usb[10] = 0x02;  // Circle Cross hat-right, L1 Options R3, touchpad
		s = ParseReport(usb, sizeof(usb), false);
		Check(s && s->lx == -32768 && s->ly == 32767 && s->rx == 32767 && s->ry == -32768, "pad: USB stick extremes, Y up = +");
		Check(s && s->l2 == 200 && s->r2 == 255, "pad: USB triggers");
		Check(s && s->buttons == (kCircle | kCross | kRight | kL1 | kOptions | kR3 | kTouchpad), "pad: USB buttons");
		bool hats = true;
		const std::uint32_t want[9] = { kUp, kUp | kRight, kRight, kDown | kRight, kDown, kDown | kLeft, kLeft, kUp | kLeft, 0 };
		for (std::uint8_t h = 0; h < 9; ++h) {
			usb[8] = h, usb[9] = 0, usb[10] = 0;
			hats = hats && ParseReport(usb, sizeof(usb), false)->buttons == want[h];
		}
		Check(hats, "pad: USB hat, all 8 directions + released");
		std::uint8_t bt[78]{ 0x31, 0x00, 0, 255, 128, 128, 7, 9, 0, 0x10 | 0x08, 0x02 | 0x04 | 0x10 | 0x40, 0x01 };
		s = ParseReport(bt, sizeof(bt), true);
		Check(s && s->lx == -32768 && s->ly == -32768 && s->l2 == 7 && s->r2 == 9 && s->buttons == (kSquare | kR1 | kL2 | kCreate | kL3 | kPS),
			"pad: Bluetooth extended (+1 offset)");
		std::uint8_t simple[10]{ 0x01, 128, 128, 128, 128, 0x80 | 0x08, 0x02, 0x02, 50, 60 };
		s = ParseReport(simple, sizeof(simple), true);
		Check(s && s->buttons == (kTriangle | kR1 | kTouchpad) && s->l2 == 50 && s->r2 == 60, "pad: Bluetooth simple");
		const std::uint8_t output[4]{ 0x02, 0, 0, 0 };
		Check(!ParseReport(output, sizeof(output), false) && !ParseReport(nullptr, 0, false) && !ParseReport(bt, sizeof(bt), false),
			"pad: other reports (and a Bluetooth report on USB) ignored");

		const auto ds = [](std::uint32_t a_buttons) { DsState d; d.buttons = a_buttons; d.l2 = 255; d.r2 = 255; d.lx = 100; d.ry = -100; return d; };
		// Menus: plain Xbox positions, triggers and sticks through.
		const auto menu = ToXInputMenu(ds(kCross | kCircle | kSquare | kTriangle | kL1 | kR1 | kOptions | kCreate | kUp));
		Check(menu.buttons == (kA | kB | kX | kY | kLeftShoulder | kRightShoulder | kStart | kBack | kDpadUp) && menu.leftTrigger == 255 &&
				  menu.thumbLX == 100 && menu.thumbRY == -100,
			"pad: menu layout = plain Xbox positions");
		DsState l1;
		l1.buttons = kL1;
		const auto tabs = ToXInputMenu(l1, true), noTabs = ToXInputMenu(l1);
		Check(tabs.leftTrigger == 255 && tabs.buttons == kLeftShoulder && noTabs.leftTrigger == 0, "pad: Journal open, L1 also pulls LT (tab switch)");
		DsState half;
		half.l2 = 20;
		half.r2 = 100;
		half.buttons = kTouchpad;
		const auto halfX = ToXInputMenu(half);
		Check(halfX.leftTrigger == 0 && halfX.rightTrigger == 255 && halfX.buttons == 0, "pad: menu triggers all-or-nothing (> 30), touchpad does nothing");
		// Gameplay: the user's layout (Skyrim events: A Activate, B Tween, X Ready Weapon, Y Jump, LB Sprint, RB Shout, Start Journal).
		const auto one = [&](std::uint32_t a_ds) { return ToXInputGameplay(ds(a_ds)).buttons; };
		Check(one(kCross) == kY && one(kTriangle) == kA && one(kSquare) == kX && one(kCircle) == kLeftShoulder && one(kOptions) == kB &&
				  one(kTouchpad) == 0 && one(kCreate) == kStart && one(kL3) == kLeftThumb && one(kR3) == kRightThumb,
			"pad: gameplay face/menu buttons (touchpad = map, handled outside XInput)");
		Check(one(kUp) == kRightShoulder && one(kDown) == kDpadUp && one(kLeft) == kDpadLeft && one(kRight) == kDpadRight,
			"pad: gameplay d-pad (up = Shout, down = Favorites)");
		const auto erOnly = ToXInputGameplay(ds(kR1 | kR2 | kL1 | kL2));
		Check(erOnly.buttons == 0 && erOnly.leftTrigger == 0 && erOnly.rightTrigger == 0 && erOnly.thumbLX == 100,
			"pad: gameplay hides R1/R2/L1/L2 from Skyrim, sticks through");
		Check(ToErButtons(ds(kR1)) == kErAttack && ToErButtons(ds(kR2)) == kErStrongAttack && ToErButtons(ds(kL1)) == kErGuard &&
				  ToErButtons(ds(kL2)) == kErSkill && ToErButtons(ds(kCross | kCircle | kTriangle | kSquare)) == 0,
			"pad: ER buttons from R1/R2/L1/L2 only");
		PadState xbox;
		xbox.buttons = kA | kB | kRightShoulder | kStart;
		xbox.rightTrigger = 200;
		xbox.leftTrigger = 20;
		const auto fromX = FromXInput(xbox);
		Check(fromX.buttons == (kCross | kCircle | kR1 | kOptions | kR2), "pad: Xbox pad mapped to DualSense positions (trigger > 30 = pressed)");
	}

	// Dev virtual pad scripts (bridge/PadScript.h).
	void PadScriptTests()
	{
		using namespace sxer::pad;
		using namespace sxer::pad::script;
		const auto e = Parse("tap Touchpad; wait 500; down R2; wait 200; up r2; stick L 1 -1; wait 100");
		Check(e && At(*e, 0).buttons == kTouchpad && At(*e, 99).buttons == kTouchpad && At(*e, 100).buttons == 0, "padscript: tap holds 100 ms");
		Check(e && At(*e, 600).buttons == kR2 && At(*e, 600).r2 == 255 && At(*e, 799).buttons == kR2, "padscript: down R2 pulls the trigger");
		Check(e && At(*e, 800).buttons == 0 && At(*e, 800).r2 == 0 && At(*e, 800).lx == 32767 && At(*e, 800).ly == -32767, "padscript: up + stick");
		Check(e && LengthMs(*e) == 900 && At(*e, 900) == DsState{}, "padscript: ends released");
		std::string error;
		Check(!Parse("tap Nope", &error) && error == "bad step: tap Nope" && !Parse("stick L 2 0") && !Parse("wait x"), "padscript: bad steps rejected");
		Check(Parse("  # comment ; tap cross 50 ;") && At(*Parse("tap cross 50"), 10).buttons == kCross, "padscript: comments, blanks, case");
	}

	void SlotTests(std::uint8_t* a_base)
	{
		using namespace sxer;
		if (!a_base) {
			Check(false, "slots: no region");
			return;
		}
		SlotReader<InputState> inputReader(a_base, proto::kOffSlotInput);
		Check(!inputReader.Read(), "slot: never written → nothing");
		SlotWriter<InputState> inputWriter(a_base, proto::kOffSlotInput);
		InputState in{};
		in.seq = 999;
		in.frame = 7;
		in.time_ms = 1234;
		in.buttons = kDodge;
		in.move_x = -0.5f;
		inputWriter.Write(in);
		const auto got = inputReader.Read();
		Check(got && got->seq == 2 && got->frame == 7 && got->time_ms == 1234 && got->buttons == kDodge && got->move_x == -0.5f,
			"slot: round trip (seq from the slot)");
		Check(Fresh(1000, 1000 + proto::kSlotStaleMs) && !Fresh(1000, 1001 + proto::kSlotStaleMs), "slot: Fresh limits");

		SlotReader<PoseState> poseReader(a_base, proto::kOffSlotPose);
		Check(!poseReader.Read(), "pose slot: never written → nothing");
		PoseState pose{};
		pose.frame = 3;
		pose.bone_count = proto::kPoseBoneCount;
		for (std::size_t i = 0; i < std::size(pose.rot); ++i) {
			pose.rot[i] = static_cast<float>(i) * 0.25f;
		}
		SlotWriter<PoseState>(a_base, proto::kOffSlotPose).Write(pose);
		const auto gotPose = poseReader.Read();
		Check(gotPose && gotPose->seq == 2 && gotPose->bone_count == proto::kPoseBoneCount && gotPose->rot[std::size(pose.rot) - 1] == (std::size(pose.rot) - 1) * 0.25f &&
				  inputReader.Read()->frame == 7,
			"pose slot: round trip, last word intact, input slot untouched");

		std::atomic<bool> stop{ false };
		std::uint64_t written = 0;
		std::thread writer([&] {
			SlotWriter<PlayerState> w(a_base, proto::kOffSlotPlayer);
			for (std::uint64_t k = 1; !stop.load(std::memory_order_relaxed); ++k) {
				w.Write(Snapshot(k));
				written = k;
				for (int spin = 0; spin < 200; ++spin) {
					::YieldProcessor();  // real writers publish once per frame
				}
			}
		});
		SlotReader<PlayerState> reader(a_base, proto::kOffSlotPlayer);
		std::uint64_t reads = 0, torn = 0, last = 0;
		bool backwards = false;
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (std::chrono::steady_clock::now() < end) {
			if (const auto s = reader.Read()) {
				torn += SameExceptSeq(*s, Snapshot(s->frame)) ? 0 : 1;
				backwards |= s->frame < last;
				last = s->frame;
				++reads;
			}
		}
		stop = true;
		writer.join();
		Check(torn == 0 && !backwards, "slot: concurrent reads never torn or out of order");
		Check(reads > 1000 && written > 1000, std::format("slot: enough traffic (reads={} written={})", reads, written).c_str());
	}

	int SelfTest()
	{
		CoordsTests();
		RigTests();
		PadTests();
		PadScriptTests();
		std::ostringstream lines;
		auto logger = std::make_shared<spdlog::logger>("selftest", std::make_shared<spdlog::sinks::ostream_sink_mt>(lines));
		logger->set_pattern("%l %v");
		spdlog::set_default_logger(logger);
		const auto has = [&](const char* a_text) { return lines.str().find(a_text) != std::string::npos; };

		const auto region = L"Local\\SkyrimXER_test_cpp_" + std::to_wstring(::GetCurrentProcessId());
		Link sky(Side::Skyrim, { { 0, 1, 0 }, { 1, 7, 104, 0 } }, region);
		Check(!sky.Shared().base.load() && !sky.Shared().connected.load(), "LinkShared empty before joining");
		{
			Link er(Side::EldenRing, { { 0, 1, 0 }, { 2, 7, 1, 0 } }, region);
			const auto both = [&](std::uint64_t a_t) {
				sky.Tick(a_t, 0);
				er.Tick(a_t, 0);
			};
			for (std::uint64_t t = 1000; t <= 1100; t += 50) {
				both(t);
			}
			Check(sky.Connected() && er.Connected(), "handshake: both connected");
			Check(sky.Shared().base.load() && sky.Shared().connected.load() && er.Shared().connected.load(), "LinkShared: region + connected");
			Check(has("CONNECTED: handshake ok with ER") && has("CONNECTED: handshake ok with Skyrim"), "handshake logged both ways");
			Check(!has("Hello again"), "exactly one Hello each way");

			for (std::uint64_t t = 1150; t <= 6250; t += 50) {
				both(t);
			}
			Check(has("ER heartbeat seq=") && has("Skyrim heartbeat seq="), "heartbeat events received both ways");

			sky.Tick(6250 + sxer::proto::kHeartbeatTimeoutMs, 0);
			Check(sky.Connected(), "no timeout at exactly the limit");
			sky.Tick(6251 + sxer::proto::kHeartbeatTimeoutMs, 0);
			Check(sky.Status() == PeerStatus::Lost && has("heartbeat timeout"), "ER stops beating → Skyrim times out");
			Check(!sky.Shared().connected.load(), "LinkShared: game threads see the loss");

			for (std::uint64_t t = 9000; t <= 9100; t += 50) {
				both(t);
			}
			Check(sky.Connected() && er.Connected(), "ER resumes → reconnected");
			Check(!has("seq gap"), "no seq gaps");
		}  // ER link destroyed: Bye + handle closed
		sky.Tick(9150, 0);
		Check(sky.Status() == PeerStatus::Lost && has("said Bye (reason=Quit)"), "ER Bye → Skyrim goes idle");
		SlotTests(sky.Shared().base.load());

		std::printf("%s", g_failures ? lines.str().c_str() : "");
		std::printf("selftest: %s\n", g_failures ? "FAILED" : "passed");
		return g_failures ? 1 : 0;
	}

	[[noreturn]] void Usage()
	{
		std::fprintf(stderr, "usage: skyrimxer_link_test selftest | peer [--side skyrim|er] [--seconds N] [--no-bye] [--region NAME] [--dodge-every S]\n");
		std::exit(2);
	}

	int Peer(int a_argc, char** a_argv)
	{
		Side side = Side::Skyrim;
		double seconds = -1;
		double dodgeEvery = 4;
		bool bye = true;
		std::wstring region = sxer::proto::kRegionName;
		for (int i = 2; i < a_argc; ++i) {
			const std::string arg = a_argv[i];
			const auto next = [&] { return i + 1 < a_argc ? std::string(a_argv[++i]) : (Usage(), std::string()); };
			if (arg == "--side") {
				const auto v = next();
				side = v == "er" ? Side::EldenRing : v == "skyrim" ? Side::Skyrim : (Usage(), Side::Skyrim);
			} else if (arg == "--seconds") {
				seconds = std::atof(next().c_str());
			} else if (arg == "--dodge-every") {
				dodgeEvery = std::atof(next().c_str());
			} else if (arg == "--no-bye") {
				bye = false;
			} else if (arg == "--region") {
				const auto v = next();
				region.assign(v.begin(), v.end());
			} else {
				Usage();
			}
		}
		const char* tag = side == Side::Skyrim ? "CPP-SKY" : "CPP-ER";
		auto logger = std::make_shared<spdlog::logger>("peer", std::make_shared<spdlog::sinks::stdout_sink_mt>());
		logger->set_pattern(std::string("%Y-%m-%dT%H:%M:%S.%eZ [") + tag + "] [%l] %v", spdlog::pattern_time_type::utc);
		logger->flush_on(spdlog::level::trace);
		spdlog::set_default_logger(logger);

		using Version4 = std::array<std::uint16_t, 4>;
		const sxer::Identity identity{ { 0, 1, 0 }, side == Side::Skyrim ? Version4{ 1, 7, 104, 0 } : Version4{ 2, 7, 1, 0 } };
		auto link = std::make_unique<Link>(side, identity, region);
		const auto& shared = link->Shared();

		// Link thread, like the plugin's Bridge.cpp.
		std::mutex lock;
		std::atomic<std::uint64_t> frames{ 0 };
		std::atomic<bool> stop{ false };
		std::thread linkThread([&] {
			while (!stop.load()) {
				{
					std::lock_guard guard(lock);
					link->Tick(sxer::NowMs(), frames.load());
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(sxer::proto::kLinkTickMs));
			}
		});

		// "Game" thread: slots only (Skyrim side; the C++ ER peer only runs the link).
		std::optional<sxer::SlotWriter<InputState>> input;
		std::optional<sxer::SlotReader<PlayerState>> player;
		std::optional<sxer::SlotReader<PoseState>> pose;
		sxer::PlayerWatch watch;
		PoseWatch poseWatch;
		std::optional<std::uint64_t> nextPulse;
		bool held = false;
		const auto everyMs = static_cast<std::uint64_t>(dodgeEvery * 1000);
		const auto start = std::chrono::steady_clock::now();
		while (seconds < 0 || std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
			const auto frame = ++frames;
			const auto now = sxer::NowMs();
			auto* base = shared.base.load(std::memory_order_acquire);
			if (side == Side::Skyrim && base && !input) {
				input.emplace(base, sxer::proto::kOffSlotInput);
				player.emplace(base, sxer::proto::kOffSlotPlayer);
				pose.emplace(base, sxer::proto::kOffSlotPose);
			}
			if (input) {
				const bool connected = shared.connected.load(std::memory_order_acquire);
				if (connected && !nextPulse) {
					nextPulse = now + 1000;
				}
				bool down = false;
				if (nextPulse && now >= *nextPulse + 250) {
					*nextPulse += everyMs;
				} else if (nextPulse) {
					down = now >= *nextPulse;
				}
				if (down != held) {
					held = down;
					spdlog::info("[input] Dodge {} frame={}", down ? "down" : "up", frame);
				}
				InputState state{};
				state.frame = frame;
				state.time_ms = now;
				state.buttons = down ? kDodge : 0;
				input->Write(state);
				watch.Update(player->Read(), connected, now);
				poseWatch.Update(pose->Read(), connected, now);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
		stop = true;
		linkThread.join();
		if (!bye) {
			spdlog::info("[link] exiting without Bye (simulated crash)");
			std::fflush(stdout);
			::TerminateProcess(::GetCurrentProcess(), 0);  // skip ~Link, which would send Bye
		}
		link.reset();
		return 0;
	}
}

int main(int a_argc, char** a_argv)
{
	if (a_argc >= 2 && std::string(a_argv[1]) == "selftest") {
		return SelfTest();
	}
	if (a_argc >= 2 && std::string(a_argv[1]) == "peer") {
		return Peer(a_argc, a_argv);
	}
	Usage();
}
