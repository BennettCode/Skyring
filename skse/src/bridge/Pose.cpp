#include "bridge/Pose.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <unordered_map>

#include "bridge/Coords.h"
#include "bridge/Rig.h"

// POSE-PLAN step 5: retarget ER's pose (model-space deltas from ER's bind, already in Skyrim's model basis) onto the player.
// Per mapped bone:  world = R_root · Yaw · delta · fit · hostBind
// - hostBind: the bone's bind rotation in the 3D root's space, from the skinned geometries' NiSkinData (inverse skinToBone; the
//   "bind = inverse skin-to-bone" idea from GTA San AnSkateas sa-plugin/src/main.cpp, re-implemented).
// - fit: rotates Skyrim's bind segment (bone → child) onto ER's (measured A-pose, docs/research/elden-ring-pose.md "Streaming it"), so
//   delta = identity shows ER's bind pose. Terminal bones reuse the parent's fit (2010-rust-rewrite-mashup skate/rig.rs idea).
// - Yaw: turns the body from Skyrim's heading to `a_facing` (heading grows = turning right = negative rotation about +Z).
// Locals come from the parents' world rotations (computed here as we go), blended with the animation's local over kBlendFrames,
// NPC COM moves for the pelvis offset and gets one UpdateDownwardPass. Writes follow SkyCraft (skse/src/Game.cpp: local write + downward pass).
namespace sxer::pose
{
	namespace
	{
		constexpr std::size_t kBones = proto::kPoseBoneCount;
		constexpr float kBlendStep = 1.0f / 5.0f;  // 5 frames in, 5 out
		constexpr float kErPelvisHeightM = 0.94f;  // ER bind/idle pelvis height above the character root (step 1)
		constexpr std::uint32_t kActive = 1u << static_cast<std::uint32_t>(proto::PoseFlag::Active);

		// PoseBone order. Note the trailing spaces in [Lft ] / [Rft ].
		constexpr std::array<const char*, kBones> kNodes = { "NPC Pelvis [Pelv]", "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",
			"NPC Neck [Neck]", "NPC Head [Head]", "NPC L Clavicle [LClv]", "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]",
			"NPC R Clavicle [RClv]", "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]", "NPC L Thigh [LThg]", "NPC L Calf [LClf]",
			"NPC L Foot [Lft ]", "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]" };
		// Segment end per bone (nullptr = terminal: parent's fit).
		constexpr std::array<const char*, kBones> kSegmentTo = { "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
			"NPC Head [Head]", nullptr, "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]", "NPC L Finger20 [LF20]",
			"NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]", "NPC R Finger20 [RF20]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]",
			"NPC L Toe0 [LToe]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]", "NPC R Toe0 [RToe]" };
		// Parent PoseBone for the terminal-bone rule (-1 = none).
		constexpr std::array<int, kBones> kParentBone = { -1, -1, 1, 2, 3, 4, 3, 6, 7, 8, 3, 10, 11, 12, 0, 14, 15, 0, 17, 18 };
		// ER bind segment directions in Skyrim's model basis (x right, y forward, z up), measured 2026-10-04 (er-plugin pose_stream.rs log).
		constexpr std::array<rig::Vec3, kBones> kErDir = { {
			{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, -0.001f, 1.0f }, { 0.0f, -0.116f, 0.993f }, { 0.0f, 0.175f, 0.985f },
			{ 0.0f, 0.0f, 1.0f },  // Head: unused (terminal)
			{ -1.0f, 0.0f, 0.0f }, { -0.707f, -0.016f, -0.707f }, { -0.705f, 0.097f, -0.703f }, { -0.735f, 0.107f, -0.670f },
			{ 1.0f, 0.0f, 0.0f }, { 0.707f, -0.016f, -0.707f }, { 0.705f, 0.097f, -0.703f }, { 0.735f, 0.107f, -0.670f },
			{ 0.0f, 0.020f, -1.0f }, { 0.0f, -0.169f, -0.986f }, { 0.0f, 0.774f, -0.634f },
			{ 0.0f, 0.020f, -1.0f }, { 0.0f, -0.169f, -0.986f }, { 0.0f, 0.774f, -0.634f } } };

		rig::Mat3 ToRig(const RE::NiMatrix3& a_m)
		{
			rig::Mat3 out{};
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					out[i][j] = a_m.entry[i][j];
				}
			}
			return out;
		}

		RE::NiMatrix3 ToNi(const rig::Mat3& a_m)
		{
			RE::NiMatrix3 out;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					out.entry[i][j] = a_m[i][j];
				}
			}
			return out;
		}

		rig::Quat ToQuat(const RE::NiMatrix3& a_m) { return rig::FromMat(ToRig(a_m)); }
		RE::NiMatrix3 ToNi(rig::Quat a_q) { return ToNi(rig::ToMat(a_q)); }
		rig::Vec3 ToVec(const RE::NiPoint3& a_p) { return { a_p.x, a_p.y, a_p.z }; }

		std::string Fmt(rig::Vec3 a_v) { return std::format("({:.2f},{:.2f},{:.2f})", a_v[0], a_v[1], a_v[2]); }

		// Bind transforms by node, in the 3D root's space: inverse skinToBone, moved from the geometry's space into the root's.
		std::unordered_map<RE::NiAVObject*, RE::NiTransform> CollectBinds(RE::NiAVObject* a_root, int& a_geometries)
		{
			std::unordered_map<RE::NiAVObject*, RE::NiTransform> binds;
			const auto rootInv = a_root->world.Invert();
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geom) {
				auto* skin = a_geom->GetGeometryRuntimeData().skinInstance.get();
				auto* data = skin ? skin->skinData.get() : nullptr;
				if (!data || !skin->bones || !data->boneData) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				++a_geometries;
				const auto geomToRoot = rootInv * a_geom->world;
				for (std::uint32_t i = 0; i < data->bones; ++i) {
					if (auto* bone = skin->bones[i]; bone && !binds.contains(bone)) {
						binds.emplace(bone, geomToRoot * data->boneData[i].skinToBone.Invert());
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return binds;
		}

		struct Rig
		{
			// Held, not just compared: a rebuilt 3D could otherwise reuse the old root's address and leave nodes[] dangling.
			RE::NiPointer<RE::NiAVObject> root;
			bool ok = false;
			std::array<RE::NiAVObject*, kBones> nodes{};
			std::array<rig::Quat, kBones> bind{};    // host bind rotation, root space
			std::array<rig::Quat, kBones> fit{};     // Skyrim bind segment → ER bind segment
			rig::Vec3 pelvisBind{};                  // root space, game units
			float heightRatio = 1.0f;                // Skyrim pelvis height / ER's
			// Common parent of Pelvis and Spine (NPC COM): moved for the pelvis offset, so the torso drops with the hips, and the
			// one node that gets the UpdateDownwardPass.
			RE::NiAVObject* mover = nullptr;
			std::array<RE::NiAVObject*, kBones> end{};  // segment end node with a bind (nullptr = terminal), for the check below
		};

		struct State
		{
			Rig rig;
			std::uint64_t retryAt = 0;  // Resolve failed: try again at this frame (geometry may still be attaching)
			float weight = 0;
			bool active = false;
			float facing = 0;                      // kept through the blend-out, so the fading pose doesn't snap round
			std::optional<proto::PoseState> last;  // pose used while blending out after it went stale
			std::uint64_t applied = 0;
			float maxUs = 0;
		};
		State g;

		// Nodes posed this frame and their world rotations (fixed size: no allocation per frame).
		struct Done
		{
			std::array<std::pair<RE::NiAVObject*, RE::NiMatrix3>, 64> items;
			std::size_t n = 0;

			const RE::NiMatrix3* Find(RE::NiAVObject* a_node) const
			{
				for (std::size_t i = 0; i < n; ++i) {
					if (items[i].first == a_node) {
						return &items[i].second;
					}
				}
				return nullptr;
			}
			void Set(RE::NiAVObject* a_node, const RE::NiMatrix3& a_rot)
			{
				for (std::size_t i = 0; i < n; ++i) {
					if (items[i].first == a_node) {
						items[i].second = a_rot;
						return;
					}
				}
				if (n < items.size()) {
					items[n++] = { a_node, a_rot };
				}
			}
		};

		bool Finite(const float* a_v, std::size_t a_n)
		{
			return std::all_of(a_v, a_v + a_n, [](float a_x) { return std::isfinite(a_x); });
		}

		bool Resolve(RE::NiAVObject* a_root)
		{
			auto& r = g.rig;
			r = Rig{};
			r.root = RE::NiPointer<RE::NiAVObject>(a_root);
			int geometries = 0;
			auto binds = CollectBinds(a_root, geometries);
			std::string missing, derived;
			std::array<std::optional<RE::NiTransform>, kBones> bind;
			for (std::size_t k = 0; k < kBones; ++k) {
				r.nodes[k] = a_root->GetObjectByName(RE::BSFixedString(kNodes[k]));
				if (!r.nodes[k]) {
					missing += std::format(" {}(node)", kNodes[k]);
					continue;
				}
				auto it = binds.find(r.nodes[k]);
				if (it == binds.end()) {
					// No mesh is skinned to it (the neck, 2026-10-04): nearest ancestor's bind · today's locals in between.
					RE::NiTransform chain;
					for (RE::NiAVObject* n = r.nodes[k]; n && n != a_root; n = n->parent) {
						if (const auto above = binds.find(n->parent); n->parent && above != binds.end()) {
							it = binds.emplace(r.nodes[k], above->second * n->local * chain).first;
							derived += std::format(" {}", kNodes[k]);
							break;
						}
						chain = n->local * chain;
					}
				}
				if (it != binds.end()) {
					bind[k] = it->second;
					r.bind[k] = ToQuat(it->second.rotate);
				} else {
					missing += std::format(" {}(bind)", kNodes[k]);
				}
			}
			if (!missing.empty()) {
				SKSE::log::warn("[pose] rig not usable ({} skinned geometries); missing:{}", geometries, missing);
				return false;
			}
			// Fits, parents first so terminal bones can reuse them.
			std::string fits;
			for (std::size_t k = 0; k < kBones; ++k) {
				std::optional<rig::Vec3> dir;
				auto er = rig::Unit(kErDir[k]);
				if (k == 0) {
					// Pelvis: Skyrim's spine sits just above and behind the pelvis (pelvis → spine fit 54°, 2026-10-04), so align
					// the hip axis instead (L Thigh → R Thigh; ER: +x).
					const auto* l = a_root->GetObjectByName(RE::BSFixedString(kNodes[14]));
					const auto* rt = a_root->GetObjectByName(RE::BSFixedString(kNodes[17]));
					const auto bl = binds.find(const_cast<RE::NiAVObject*>(l));
					const auto br = binds.find(const_cast<RE::NiAVObject*>(rt));
					if (bl != binds.end() && br != binds.end()) {
						dir = rig::Unit(rig::Sub(ToVec(br->second.translate), ToVec(bl->second.translate)));
					}
					er = rig::Vec3{ 1.0f, 0.0f, 0.0f };
				} else if (kSegmentTo[k]) {
					if (auto* end = a_root->GetObjectByName(RE::BSFixedString(kSegmentTo[k]))) {
						if (const auto it = binds.find(end); it != binds.end()) {
							r.end[k] = end;
							dir = rig::Unit(rig::Sub(ToVec(it->second.translate), ToVec(bind[k]->translate)));
						}
					}
				}
				if (dir && er) {
					r.fit[k] = rig::RotationArc(*dir, *er);
				} else {
					r.fit[k] = kParentBone[k] >= 0 ? r.fit[kParentBone[k]] : rig::kIdentity;
				}
				fits += std::format(" {}={:.0f}", k, rig::Angle(r.fit[k]) * 57.2958f);
			}
			// Pelvis and Spine both hang off NPC COM (docs/research/skyrim-hooks.md): moving it moves the whole body.
			r.mover = r.nodes[0]->parent;
			bool spineUnder = false;
			for (auto* p = r.nodes[1]->parent; p && !spineUnder; p = p->parent) {
				spineUnder = p == r.mover;
			}
			if (!r.mover || !r.mover->parent || !spineUnder) {
				SKSE::log::warn("[pose] rig not usable: the pelvis's parent '{}' is not above the spine", r.mover ? r.mover->name.c_str() : "none");
				return false;
			}
			r.pelvisBind = ToVec(bind[0]->translate);
			r.heightRatio = r.pelvisBind[2] / (kErPelvisHeightM * coords::kSkyrimUnitsPerM);
			const auto pos = [&](std::size_t k) { return Fmt(ToVec(bind[k]->translate)); };
			SKSE::log::info("[pose] rig: {} bones from {} skinned geometries; bind (root space, units): Pelvis={} Head={} L_Hand={} R_Hand={} "
							"L_Foot={}; height ratio {:.3f}",
				kBones, geometries, pos(0), pos(5), pos(9), pos(13), pos(16), r.heightRatio);
			SKSE::log::info("[pose] fit angles (deg, PoseBone order):{}; bind derived from the parent chain:{}", fits, derived.empty() ? " none" : derived);
			return r.ok = true;
		}

		// World rotation of a_node this frame: ours if we posed it (or an ancestor of it), else the animation's.
		RE::NiMatrix3 WorldRot(RE::NiAVObject* a_node, Done& a_done)
		{
			if (const auto* rot = a_done.Find(a_node)) {
				return *rot;
			}
			bool posedAbove = false;
			for (auto* p = a_node->parent; p && !posedAbove; p = p->parent) {
				posedAbove = a_done.Find(p) != nullptr;
			}
			if (!posedAbove || !a_node->parent) {
				return a_node->world.rotate;
			}
			auto rot = WorldRot(a_node->parent, a_done) * a_node->local.rotate;
			a_done.Set(a_node, rot);
			return rot;
		}
	}

	void Apply(RE::PlayerCharacter* a_player, const std::optional<proto::PoseState>& a_pose, float a_facing, std::uint64_t a_frame)
	{
		const auto start = std::chrono::steady_clock::now();
		auto* root = a_player ? a_player->Get3D(false) : nullptr;
		if (!root) {
			return;
		}
		if (root != g.rig.root.get() || (!g.rig.ok && a_frame >= g.retryAt)) {
			if (!Resolve(root)) {
				g.retryAt = a_frame + 60;
			}
			g.weight = 0;
		}
		if (!g.rig.ok) {
			return;
		}
		const auto* camera = RE::PlayerCamera::GetSingleton();
		const bool firstPerson = camera && camera->IsInFirstPerson();
		// The ER writer already rejects non-finite poses; a mismatched DLL must still never write NaN positions into the skeleton.
		const bool valid = a_pose && a_pose->bone_count == kBones && Finite(a_pose->pelvis_offset, 3) && Finite(a_pose->rot, std::size(a_pose->rot));
		const bool active = valid && (a_pose->flags & kActive) && !firstPerson;
		if (active) {
			g.last = a_pose;
			g.facing = a_facing;
		}
		if (active != g.active) {
			g.active = active;
			if (active) {
				SKSE::log::info("[pose] apply on er_frame={} facing={:.3f} body={:.3f} frame={}", a_pose->frame, a_facing, a_player->GetAngleZ(), a_frame);
				g.applied = 0;
				g.maxUs = 0;
			} else {
				SKSE::log::info("[pose] apply off frame={} (posed frames={}, max {:.0f} us)", a_frame, g.applied, g.maxUs);
			}
		}
		g.weight = std::clamp(g.weight + (active ? kBlendStep : -kBlendStep), 0.0f, 1.0f);
		if (g.weight <= 0 || !g.last) {
			return;
		}
		const auto& pose = *g.last;
		const auto& r = g.rig;

		const auto& rootRot = root->world.rotate;
		// Yaw from the root's actual heading (model +Y → world (sin h, cos h)), not GetAngleZ(): the two can differ for a frame after
		// SetHeading. Heading grows when turning right = negative rotation about +Z.
		const float rootHeading = std::atan2(rootRot.entry[0][1], rootRot.entry[1][1]);
		const auto yaw = rig::AxisAngle({ 0.0f, 0.0f, 1.0f }, -std::remainder(g.facing - rootHeading, 2.0f * 3.14159265f));
		Done done;
		for (std::size_t k = 0; k < kBones; ++k) {
			auto* node = r.nodes[k];
			const auto parentWorld = node->parent ? WorldRot(node->parent, done) : RE::NiMatrix3{};
			if (const auto delta = rig::Normalize({ pose.rot[k * 4], pose.rot[k * 4 + 1], pose.rot[k * 4 + 2], pose.rot[k * 4 + 3] })) {
				const auto world = rootRot * ToNi(rig::Mul(yaw, rig::Mul(rig::Mul(*delta, r.fit[k]), r.bind[k])));
				const auto target = ToQuat(parentWorld.Transpose() * world);
				node->local.rotate = ToNi(rig::Slerp(ToQuat(node->local.rotate), target, g.weight));
			}
			done.Set(node, parentWorld * node->local.rotate);
		}
		// Pelvis position: bind + ER's offset (metres → units, scaled to this body), turned by the same yaw. COM (the pelvis's parent,
		// also the spine's) is moved by the difference, so hips and torso drop together; COM's rotation is untouched, so the
		// rotations above stay valid.
		{
			const float s = coords::kSkyrimUnitsPerM * r.heightRatio;
			const rig::Vec3 model = rig::Rotate(yaw, { r.pelvisBind[0] + pose.pelvis_offset[0] * s, r.pelvisBind[1] + pose.pelvis_offset[1] * s,
														 r.pelvisBind[2] + pose.pelvis_offset[2] * s });
			const auto target = root->world.translate + rootRot * RE::NiPoint3(model[0], model[1], model[2]) * root->world.scale;
			const auto& above = r.mover->parent->world;
			const auto shift = above.rotate.Transpose() * (target - r.nodes[0]->world.translate) / above.scale;
			r.mover->local.translate = r.mover->local.translate + shift * g.weight;
		}
		RE::NiUpdateData update{};
		r.mover->UpdateDownwardPass(update, 0);
		// Check (first frames of each stretch, then every 300): the posed segment (bone → end) in model space vs ER's, after the yaw
		// and delta. Big errors mean a wrong bind, fit or basis; the numbers say which bone.
		if (g.weight >= 1.0f && (g.applied == 10 || g.applied % 300 == 299)) {
			std::string line;
			for (std::size_t k = 0; k < kBones; ++k) {
				if (!r.end[k] || k == 0) {
					continue;
				}
				const auto d = rootRot.Transpose() * (r.end[k]->world.translate - r.nodes[k]->world.translate);
				const auto got = rig::Unit({ d.x, d.y, d.z });
				const rig::Quat delta{ pose.rot[k * 4], pose.rot[k * 4 + 1], pose.rot[k * 4 + 2], pose.rot[k * 4 + 3] };
				const auto want = rig::Unit(rig::Rotate(rig::Mul(yaw, delta), kErDir[k]));
				if (got && want) {
					line += std::format(" {}={:.0f}", k, std::acos(std::clamp(rig::Dot(*got, *want), -1.0f, 1.0f)) * 57.2958f);
				}
			}
			SKSE::log::info("[pose] check: segment error deg (PoseBone order):{} frame={}", line, a_frame);
		}
		++g.applied;
		g.maxUs = std::max(g.maxUs, std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count());
	}
}
