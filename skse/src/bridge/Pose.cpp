#include "bridge/Pose.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bridge/Coords.h"
#include "bridge/Rig.h"

// Retarget ER's pose (model-space deltas from ER's bind, already in Skyrim's model basis) onto the player (POSE-PLAN step 5,
// LOCO-PLAN stage A). Per mapped bone:  world = R_root · Yaw · delta · fit · hostBind
// - hostBind: the bone's bind rotation in the 3D root's space, from the skinned geometries' NiSkinData (inverse skinToBone; the
//   "bind = inverse skin-to-bone" idea from GTA San AnSkateas sa-plugin/src/main.cpp, re-implemented).
// - fit (limbs only): rotates Skyrim's bind segment (bone → child) onto ER's, from the PoseBind slot, so arms and legs leave ER's
//   A-pose the way ER's do. Shape bones (pelvis, spine, neck, head, clavicles) keep Skyrim's own bind shape: a fit there moved the
//   shoulder joints and stretched the chest (2026-10-04 playtest). Bones without a segment (hands, feet, twist bones) reuse their
//   parent's fit (2010-rust-rewrite-mashup skate/rig.rs idea). Hands instead fit two directions, hand → middle finger and hand →
//   thumb, against ER's (PoseBind dir + thumb): a segment alone leaves the roll free, and the hand was rolled so far that the sword
//   pointed back along the forearm like a dagger (2026-10-05).
// - Weapons: the right hand's WEAPON node turns so the weapon's blade points along ER's (AimWeapon, PoseState.blade); a bow moves to
//   the right hand except while ER draws it (CarryBow, PoseFlag BowLeft).
// - Yaw: turns the body from Skyrim's heading to `a_facing` (heading grows = turning right = negative rotation about +Z).
// - Unmapped skinned helpers hanging off a posed bone (pauldrons...) keep their bind offset to it while posed (mashup rule: unmapped
//   bones inherit), so the mesh around them doesn't follow Skyrim's animation.
// Locals come from the parents' world rotations (computed as we go), blended with the animation's local over 5 frames. NPC COM
// moves for the pelvis offset and gets one UpdateDownwardPass. Writes follow SkyCraft (skse/src/Game.cpp: local write + downward pass).
namespace sxer::pose
{
	namespace
	{
		constexpr std::size_t kBones = proto::kPoseBoneCount;
		constexpr float kBlendStep = 1.0f / 5.0f;  // 5 frames in, 5 out
		constexpr float kErPelvisHeightM = 0.94f;  // ER bind/idle pelvis height above the character root (step 1)
		constexpr std::uint32_t kActive = 1u << static_cast<std::uint32_t>(proto::PoseFlag::Active);

		// PoseBone order. Note the trailing spaces in [Lft ] / [Rft ]. The player's skeleton has no finger or toe bones (51 nodes).
		constexpr std::array<const char*, kBones> kNodes = { "NPC Pelvis [Pelv]", "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]",
			"NPC Neck [Neck]", "NPC Head [Head]", "NPC L Clavicle [LClv]", "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]",
			"NPC R Clavicle [RClv]", "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]", "NPC L Thigh [LThg]", "NPC L Calf [LClf]",
			"NPC L Foot [Lft ]", "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]", "NPC L UpperarmTwist1 [LUt1]",
			"NPC L UpperarmTwist2 [LUt2]", "NPC R UpperarmTwist1 [RUt1]", "NPC R UpperarmTwist2 [RUt2]" };
		// Segment end per bone (nullptr = none).
		constexpr std::array<const char*, kBones> kSegmentTo = { "NPC Spine [Spn0]", "NPC Spine1 [Spn1]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]",
			"NPC Head [Head]", nullptr, "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]", nullptr, "NPC R UpperArm [RUar]",
			"NPC R Forearm [RLar]", "NPC R Hand [RHnd]", nullptr, "NPC L Calf [LClf]", "NPC L Foot [Lft ]", nullptr, "NPC R Calf [RClf]",
			"NPC R Foot [Rft ]", nullptr, nullptr, nullptr, nullptr, nullptr };
		// Parent PoseBone (-1 = none), for the shared-fit rule.
		constexpr std::array<int, kBones> kParentBone = { -1, -1, 1, 2, 3, 4, 3, 6, 7, 8, 3, 10, 11, 12, 0, 14, 15, 0, 17, 18, 7, 7, 11, 11 };
		// Spine, Spine1, Spine2: shape bones whose delta is conjugated by their bind fit (fit⁻¹·delta·fit). They keep Skyrim's bind shape
		// (no posture change at rest), but ER's rotation turns about Skyrim's own segment instead of ER's. Applied unchanged, ER's sprint
		// chest twist swung Skyrim's Spine2 (14 deg further forward than ER's in bind) round in a cone: the upper chest swayed +-22 deg
		// sideways against ER's +-9, the head +-15 cm against +-7 ([bodydump] probe 2026-10-05).
		constexpr std::array<bool, kBones> kConj = { false, true, true, true };
		// Limbs get a fit; shape bones keep Skyrim's bind shape.
		constexpr std::array<bool, kBones> kLimb = { false, false, false, false, false, false, false, true, true, true, false, true, true, true,
			true, true, true, true, true, true, true, true, true, true };

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

		// An unmapped skinned bone under a posed one: held at its bind offset while posed.
		struct Helper
		{
			RE::NiAVObject* node;
			rig::Quat bindLocal;
		};

		struct Rig
		{
			// Held, not just compared: a rebuilt 3D could otherwise reuse the old root's address and leave nodes[] dangling.
			RE::NiPointer<RE::NiAVObject> root;
			bool ok = false;
			std::array<RE::NiAVObject*, kBones> nodes{};
			std::array<rig::Quat, kBones> bind{};  // host bind rotation, root space
			std::array<std::optional<rig::Vec3>, kBones> skyDir{};  // Skyrim bind segment direction (unit), root space
			std::array<RE::NiAVObject*, kBones> end{};             // segment end node with a bind, for the check line
			std::array<rig::Quat, kBones> fit{};    // from PoseBind (identity until it arrives / for shape bones)
			std::array<rig::Quat, kBones> conj{};   // spine bones: ER's delta is seen in Skyrim's bind frame (fit⁻¹·delta·fit), see kConj
			std::array<rig::Vec3, kBones> erDir{};  // ER bind segment directions (PoseBind), for the check line
			// Hands (left, right): Skyrim bind directions hand → middle finger base and hand → thumb base, root space.
			std::array<std::optional<rig::Vec3>, 2> skyFinger{}, skyThumb{};
			std::uint64_t bindFrame = 0;            // PoseBind.frame the fits came from (0 = none yet: don't pose)
			rig::Vec3 pelvisBind{};                 // root space, game units
			float heightRatio = 1.0f;               // Skyrim pelvis height / ER's
			// Common parent of Pelvis and Spine (NPC COM): moved for the pelvis offset, so the torso drops with the hips, and the
			// one node that gets the UpdateDownwardPass.
			RE::NiAVObject* mover = nullptr;
			std::vector<Helper> helpers;
			std::unordered_set<const RE::NiTransform*> treeWorlds;  // world transforms of every node in the third-person tree
			RE::NiAVObject* weapon = nullptr;  // WEAPON (right hand's attach node), see AimWeapon
			RE::NiAVObject* shield = nullptr;  // SHIELD (left hand's; bows hang here), see CarryBow
		};

		// A hand attach node we move (WEAPON, SHIELD): its local before we moved it (the animation's, or the helper rule's) and what we
		// last wrote. Anything else found there means someone else wrote it: that becomes the new rest.
		struct Moved
		{
			std::optional<RE::NiTransform> rest, written;
		};

		struct State
		{
			Rig rig;
			std::unordered_set<const RE::NiTransform*> treeNow;  // Carry: the tree's world transforms this frame (reused buffer)
			std::uint64_t retryAt = 0;  // Resolve failed: try again at this frame (geometry may still be attaching)
			float weight = 0;
			bool active = false;
			float facing = 0;                      // kept through the blend-out, so the fading pose doesn't snap round
			std::optional<proto::PoseState> last;  // pose used while blending out after it went stale
			std::uint64_t applied = 0;
			float maxUs = 0;
			Moved weaponMoved, bowMoved;  // AimWeapon (WEAPON), CarryBow (SHIELD)
		};
		State g;

		// [body] telemetry (2026-10-05: run/sprint body direction and lean looked off): the posed body's facing and lean relative to where
		// the player travels, in the same terms as ER's [body] line (er-plugin/src/body.rs), so the sides compare without matching clocks.
		// Degrees; turning right, leaning forward and leaning right are positive. One line per 120 frames of movement (mean/largest).
		struct BodyStat
		{
			float sum = 0;
			float max = 0;  // the value with the largest magnitude, signed
			void Add(float a_rad)
			{
				const float v = a_rad * 57.2958f;
				sum += v;
				max = std::fabs(v) > std::fabs(max) ? v : max;
			}
			std::string Show(int a_n) const { return std::format("{:.0f}/{:.0f}", sum / a_n, max); }
		};
		struct Body
		{
			std::optional<RE::NiPoint3> last;
			std::chrono::steady_clock::time_point lastAt;
			int n = 0;
			BodyStat face, actor, hip, chest, leanF, leanS;
		} body;

		float WrapPi(float a_angle) { return std::remainder(a_angle, 2.0f * 3.14159265f); }

		void MeasureBody(RE::PlayerCharacter* a_player, RE::NiAVObject* a_root)
		{
			constexpr float kMinSpeed = 0.5f * coords::kSkyrimUnitsPerM;  // units/s; slower frames have no travel direction
			const auto now = std::chrono::steady_clock::now();
			const auto here = a_root->world.translate;
			const auto last = std::exchange(body.last, here);
			const float dt = std::chrono::duration<float>(now - std::exchange(body.lastAt, now)).count();
			if (!last || dt <= 0 || dt > 0.1f) {
				return;
			}
			const float dx = here.x - last->x, dy = here.y - last->y;
			if (std::hypot(dx, dy) / dt < kMinSpeed) {
				return;
			}
			const float travel = std::atan2(dx, dy);  // heading: forward = (sin h, cos h)
			const auto& r = g.rig;
			const auto at = [&](std::size_t k) { return r.nodes[k]->world.translate; };
			// Axis L → R as a heading (right = (cos h, −sin h)), relative to travel.
			const auto axis = [&](std::size_t l, std::size_t rt) {
				const auto v = at(rt) - at(l);
				return WrapPi(std::atan2(-v.y, v.x) - travel);
			};
			++body.n;
			body.face.Add(WrapPi(g.facing - travel));
			body.actor.Add(WrapPi(a_player->GetAngleZ() - travel));
			body.hip.Add(axis(14, 17));   // L Thigh → R Thigh
			body.chest.Add(axis(7, 11));  // L UpperArm → R UpperArm
			const auto v = at(4) - at(0);  // pelvis → neck
			const float s = std::sin(travel), c = std::cos(travel);
			body.leanF.Add(std::atan2(v.x * s + v.y * c, v.z));
			body.leanS.Add(std::atan2(v.x * c - v.y * s, v.z));
			if (body.n >= 120) {
				const int n = body.n;
				SKSE::log::info("[body] n={} deg mean/max vs travel: face={} actor={} hip={} chest={} leanF={} leanS={}", n, body.face.Show(n),
					body.actor.Show(n), body.hip.Show(n), body.chest.Show(n), body.leanF.Show(n), body.leanS.Show(n));
				body = Body{ .last = body.last, .lastAt = body.lastAt };
			}
		}

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

		bool IsMapped(const Rig& a_r, const RE::NiAVObject* a_node)
		{
			return std::find(a_r.nodes.begin(), a_r.nodes.end(), a_node) != a_r.nodes.end();
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
			for (std::size_t k = 0; k < kBones; ++k) {
				if (!kSegmentTo[k]) {
					continue;
				}
				if (auto* end = a_root->GetObjectByName(RE::BSFixedString(kSegmentTo[k]))) {
					if (const auto it = binds.find(end); it != binds.end()) {
						r.end[k] = end;
						r.skyDir[k] = rig::Unit(rig::Sub(ToVec(it->second.translate), ToVec(bind[k]->translate)));
					}
				}
			}
			// Finger bases: neither tree nodes nor bind entries (the skins' bone pointers for them are null), so their offsets come from the
			// animation skeleton (Havok reference pose). There they are the hand's children: the local translation is the hand-local
			// offset, turned into root space by the hand's bind rotation.
			{
				RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
				const RE::hkaSkeleton* skeleton = nullptr;
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (player && player->GetAnimationGraphManager(manager) && manager && manager->graphs.size() > 0 && manager->graphs[0]) {
					if (const auto* setup = manager->graphs[0]->characterInstance.setup.get()) {
						skeleton = setup->animationSkeleton.get();
					}
				}
				const auto boneIndex = [&](const char* a_name) -> int {
					for (int i = 0; skeleton && i < skeleton->bones.size(); ++i) {
						if (const char* n = skeleton->bones[i].name.c_str(); n && std::strcmp(n, a_name) == 0) {
							return i;
						}
					}
					return -1;
				};
				constexpr std::array<std::array<const char*, 3>, 2> kHandRefs = { { { "NPC L Hand [LHnd]", "NPC L Finger20 [LF20]", "NPC L Finger00 [LF00]" },
					{ "NPC R Hand [RHnd]", "NPC R Finger20 [RF20]", "NPC R Finger00 [RF00]" } } };
				for (std::size_t h = 0; h < 2 && skeleton && skeleton->referencePose.size() == skeleton->bones.size() &&
										skeleton->parentIndices.size() == skeleton->bones.size();
					 ++h) {
					const int hand = boneIndex(kHandRefs[h][0]);
					const auto handRot = ToRig(bind[h == 0 ? 9 : 13]->rotate);
					const auto offset = [&](const char* a_name) -> std::optional<rig::Vec3> {
						const int i = boneIndex(a_name);
						if (i < 0 || hand < 0 || skeleton->parentIndices[i] != hand) {
							return std::nullopt;
						}
						const auto& t = skeleton->referencePose[i].translation.quad.m128_f32;
						rig::Vec3 v{};
						for (int r = 0; r < 3; ++r) {
							v[r] = handRot[r][0] * t[0] + handRot[r][1] * t[1] + handRot[r][2] * t[2];
						}
						return rig::Unit(v);
					};
					r.skyFinger[h] = offset(kHandRefs[h][1]);
					r.skyThumb[h] = offset(kHandRefs[h][2]);
				}
				if (!skeleton) {
					SKSE::log::warn("[pose] no animation skeleton: hands keep the forearm's fit");
				}
				const auto opt = [](const std::optional<rig::Vec3>& a_v) { return a_v ? Fmt(*a_v) : std::string("missing"); };
				SKSE::log::info("[pose] hand bind dirs (root space, unit): L finger {} thumb {}, R finger {} thumb {}", opt(r.skyFinger[0]),
					opt(r.skyThumb[0]), opt(r.skyFinger[1]), opt(r.skyThumb[1]));
			}
			r.weapon = a_root->GetObjectByName(RE::BSFixedString("WEAPON"));
			r.shield = a_root->GetObjectByName(RE::BSFixedString("SHIELD"));
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
			// Helpers: skinned, unmapped, directly under a posed bone.
			std::string helpers;
			for (const auto& [node, t] : binds) {
				auto* parent = node->parent;
				if (!parent || IsMapped(r, node) || !IsMapped(r, parent)) {
					continue;
				}
				if (const auto pb = binds.find(parent); pb != binds.end()) {
					r.helpers.push_back({ node, rig::Mul(rig::Conj(ToQuat(pb->second.rotate)), ToQuat(t.rotate)) });
					helpers += std::format(" {}", node->name.c_str());
				}
			}
			// Third-person scene-graph transforms: anything a skinned mesh skins with that isn't one of these is a loose animation output
			// (fingers, toes: see Carry), 2026-10-05 probe.
			{
				std::vector<RE::NiAVObject*> stack{ a_root };
				while (!stack.empty()) {
					auto* n = stack.back();
					stack.pop_back();
					r.treeWorlds.insert(&n->world);
					if (auto* node = n->AsNode()) {
						for (auto& c : node->GetChildren()) {
							if (c) {
								stack.push_back(c.get());
							}
						}
					}
				}
			}
			r.pelvisBind = ToVec(bind[0]->translate);
			r.heightRatio = r.pelvisBind[2] / (kErPelvisHeightM * coords::kSkyrimUnitsPerM);
			const auto pos = [&](std::size_t k) { return Fmt(ToVec(bind[k]->translate)); };
			SKSE::log::info("[pose] rig: {} bones from {} skinned geometries; bind (root space, units): Pelvis={} Head={} L_Hand={} R_Hand={} "
							"L_Foot={}; height ratio {:.3f}; bind derived:{}",
				kBones, geometries, pos(0), pos(5), pos(9), pos(13), pos(16), r.heightRatio, derived.empty() ? " none" : derived);
			SKSE::log::info("[pose] helpers held at bind ({}):{}", r.helpers.size(), helpers.empty() ? " none" : helpers);
			return r.ok = true;
		}

		// Fingers and toes aren't nodes in the player's tree (51 nodes): the skinned hands, feet and body skin them through loose world
		// transforms the animation writes directly (boneWorldTransforms entries outside the tree, 0x80 apart, at the hands and feet;
		// probe 2026-10-05). Re-posing a hand left its fingers where Skyrim's animation put them: hands, wrists and feet stretched.
		// Each frame, every such transform is carried rigidly with the nearest posed bone (its change from the animation's world to ours),
		// so fingers keep Skyrim's grip relative to the hand. Pointers are read from the live skin instances every frame (an equipment
		// change rebuilds them).
		void Carry(RE::NiAVObject* a_root, const std::array<RE::NiTransform, kBones>& a_before)
		{
			const auto& r = g.rig;
			std::array<RE::NiTransform, kBones> change;
			for (std::size_t k = 0; k < kBones; ++k) {
				change[k] = r.nodes[k]->world * a_before[k].Invert();
			}
			// Tree nodes now, not at rig time: equipping a weapon adds nodes, and a skinned weapon (bows: Bow_MidBone, Bow_UpBone...
			// under the hand's Weapon node) skins with them. Treated as loose, they were moved a second time with the nearest bone of
			// Skyrim's animation and the bow hung at the hip (2026-10-05). They already follow the hand through the tree.
			auto& tree = g.treeNow;
			tree.clear();
			{
				std::vector<RE::NiAVObject*> stack{ a_root };
				while (!stack.empty()) {
					auto* n = stack.back();
					stack.pop_back();
					tree.insert(&n->world);
					if (auto* node = n->AsNode()) {
						for (auto& c : node->GetChildren()) {
							if (c) {
								stack.push_back(c.get());
							}
						}
					}
				}
			}
			std::array<const RE::NiTransform*, 256> seen{};
			std::size_t nSeen = 0;
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geom) {
				auto* skin = a_geom->GetGeometryRuntimeData().skinInstance.get();
				auto* data = skin ? skin->skinData.get() : nullptr;
				if (!data || !skin->boneWorldTransforms) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				for (std::uint32_t i = 0; i < data->bones; ++i) {
					const auto* t = skin->boneWorldTransforms[i];
					if (!t || tree.contains(t) || std::find(seen.begin(), seen.begin() + nSeen, t) != seen.begin() + nSeen) {
						continue;
					}
					if (nSeen < seen.size()) {
						seen[nSeen++] = t;
					}
					std::size_t best = 0;
					float bestD = 1e30f;
					for (std::size_t k = 0; k < kBones; ++k) {
						const float d = (a_before[k].translate - t->translate).SqrLength();
						if (d < bestD) {
							bestD = d, best = k;
						}
					}
					*const_cast<RE::NiTransform*>(t) = change[best] * *t;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		// Fits from ER's bind directions (PoseBind), limbs only.
		void Refit(const proto::PoseBind& a_bind)
		{
			auto& r = g.rig;
			std::string line;
			for (std::size_t k = 0; k < kBones; ++k) {
				r.erDir[k] = { a_bind.dir[k * 3], a_bind.dir[k * 3 + 1], a_bind.dir[k * 3 + 2] };
				const auto er = rig::Unit(r.erDir[k]);
				if (!kLimb[k]) {
					r.fit[k] = rig::kIdentity;
				} else if (r.skyDir[k] && er) {
					r.fit[k] = rig::RotationArc(*r.skyDir[k], *er);
				} else {
					r.fit[k] = kParentBone[k] >= 0 ? r.fit[kParentBone[k]] : rig::kIdentity;
				}
				line += std::format(" {}={:.0f}", k, rig::Angle(r.fit[k]) * 57.2958f);
				if (k == 9 || k == 13) {
					const std::size_t h = k == 9 ? 0 : 1;
					const auto erThumb = rig::Unit({ a_bind.thumb[h * 3], a_bind.thumb[h * 3 + 1], a_bind.thumb[h * 3 + 2] });
					const auto hand = r.skyFinger[h] && r.skyThumb[h] && er && erThumb ? rig::FrameFit(*r.skyFinger[h], *r.skyThumb[h], *er, *erThumb)
					                                                                   : std::nullopt;
					if (hand) {
						line += std::format(" (roll fix {:.0f})", rig::Angle(rig::Mul(*hand, rig::Conj(r.fit[k]))) * 57.2958f);
						r.fit[k] = *hand;
						line += std::format(" ={:.0f}", rig::Angle(r.fit[k]) * 57.2958f);
					} else {
						line += " (no hand fit)";
					}
				}
				r.conj[k] = kConj[k] && r.skyDir[k] && er ? rig::RotationArc(*r.skyDir[k], *er) : rig::kIdentity;
				if (kConj[k]) {
					line += std::format(" (conj {:.0f})", rig::Angle(r.conj[k]) * 57.2958f);
				}
			}
			r.bindFrame = a_bind.frame;
			SKSE::log::info("[pose] PoseBind er_frame={}: fit angles (deg, PoseBone order; shape bones 0, spine conjugation in brackets):{}", a_bind.frame, line);
		}

		bool Same(const RE::NiTransform& a_a, const RE::NiTransform& a_b)
		{
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					if (std::fabs(a_a.rotate.entry[i][j] - a_b.rotate.entry[i][j]) > 1e-4f) {
						return false;
					}
				}
			}
			return (a_a.translate - a_b.translate).SqrLength() < 1e-6f;
		}

		// Puts a moved attach node back as the animation / skeleton had it (blend-out finished, or nothing to do).
		void Release(RE::NiAVObject* a_node, Moved& a_m)
		{
			if (a_node && a_m.rest && a_m.written && Same(a_node->local, *a_m.written)) {
				a_node->local = *a_m.rest;
				RE::NiUpdateData update{};
				a_node->UpdateDownwardPass(update, 0);
			}
			a_m = {};
		}

		// Rest = what the animation / skeleton put there this frame (if what's there isn't ours, someone rewrote it). Returns the world
		// transform the node has under its rest local.
		RE::NiTransform Begin(RE::NiAVObject* a_node, Moved& a_m)
		{
			if (!a_m.rest || !a_m.written || !Same(a_node->local, *a_m.written)) {
				a_m.rest = a_node->local;
			}
			return a_node->parent->world * *a_m.rest;
		}

		// Moves a_node to a world rotation and position through its local, blended from its rest with the pose weight.
		void MoveTo(RE::NiAVObject* a_node, Moved& a_m, const RE::NiMatrix3& a_worldRot, const RE::NiPoint3& a_worldPos)
		{
			const auto& parent = a_node->parent->world;
			auto out = *a_m.rest;
			out.rotate = ToNi(rig::Slerp(ToQuat(a_m.rest->rotate), ToQuat(parent.rotate.Transpose() * a_worldRot), g.weight));
			const auto local = parent.rotate.Transpose() * (a_worldPos - parent.translate) / parent.scale;
			out.translate = a_m.rest->translate + (local - a_m.rest->translate) * g.weight;
			a_node->local = out;
			a_m.written = out;
			RE::NiUpdateData update{};
			a_node->UpdateDownwardPass(update, 0);
		}

		// ER's direction a_v (model space, Skyrim basis) in the world this frame.
		RE::NiPoint3 ErWorldDir(rig::Vec3 a_v, const RE::NiMatrix3& a_rootRot, rig::Quat a_yaw)
		{
			const auto b = rig::Rotate(a_yaw, a_v);
			return a_rootRot * RE::NiPoint3(b[0], b[1], b[2]);
		}

		std::optional<rig::Vec3> UnitOf(const RE::NiPoint3& a_p) { return rig::Unit({ a_p.x, a_p.y, a_p.z }); }

		// ER holds its weapon at a fixed angle in the hand (R_Weapon in R_Hand's frame stayed constant through idle, walk and most of a
		// swing, probe 2026-10-06) that differs from Skyrim's WEAPON node: a posed hand held a Skyrim sword pointing up where ER's points
		// forward/down ("like a dagger"). So WEAPON turns (shortest arc) until its weapon's blade, node → the weapon mesh's bound
		// centre, points along ER's R_Weapon +Y. Run after the hands are posed and updated. Bows (bound centre at the grip): CarryBow.
		void AimWeapon(const proto::PoseState& a_pose, const RE::NiMatrix3& a_rootRot, rig::Quat a_yaw)
		{
			auto* node = g.rig.weapon;
			const auto blade = rig::Unit({ a_pose.blade[3], a_pose.blade[4], a_pose.blade[5] });
			RE::NiAVObject* mesh = nullptr;
			if (auto* n = node ? node->AsNode() : nullptr) {
				for (auto& c : n->GetChildren()) {
					if (c && c->worldBound.radius > 1.0f) {
						mesh = c.get();
						break;
					}
				}
			}
			const auto offset = mesh ? node->world.rotate.Transpose() * (mesh->worldBound.center - node->world.translate) / node->world.scale : RE::NiPoint3{};
			const auto local = UnitOf(offset);
			if (!node || !node->parent || !blade || !local || offset.Length() < 6.0f) {
				Release(node, g.weaponMoved);
				return;
			}
			const auto rest = Begin(node, g.weaponMoved);
			const auto now = UnitOf(rest.rotate * RE::NiPoint3((*local)[0], (*local)[1], (*local)[2]));
			const auto want = UnitOf(ErWorldDir(*blade, a_rootRot, a_yaw));
			const auto arc = rig::RotationArc(now.value_or(*local), want.value_or(*local));
			if (g.applied % 120 == 0) {
				SKSE::log::info("[pose] weapon aim: blade (node space) {} turned {:.0f} deg onto ER's {} (model)", Fmt(*local),
					rig::Angle(arc) * 57.2958f, Fmt(*blade));
			}
			MoveTo(node, g.weaponMoved, ToNi(arc) * rest.rotate, rest.translate);
		}

		// ER carries a bow in the right hand and takes it into the left only to draw, hold and release (contact sheet 2026-10-06: idle
		// and running hold it sideways in the right hand). Skyrim keeps it on SHIELD under the left hand, so the left arm swung a bow ER
		// doesn't hold ("running with the bow is weird"). While ER's BowLeft flag is off, SHIELD moves to the right hand's WEAPON node,
		// turned (shortest arc) so the bow's upper limb (Bow_MidBone → Bow_UpBone) points along ER's R_Weapon -Y: at full draw ER's
		// L_Weapon +Y points straight down, so +Y is the lower limb.
		void CarryBow(const proto::PoseState& a_pose, const RE::NiMatrix3& a_rootRot, rig::Quat a_yaw)
		{
			auto* node = g.rig.shield;
			auto* grip = g.rig.weapon;
			RE::NiAVObject* mid = nullptr;
			RE::NiAVObject* up = nullptr;
			if (node) {
				mid = node->GetObjectByName(RE::BSFixedString("Bow_MidBone"));
				up = node->GetObjectByName(RE::BSFixedString("Bow_UpBone2"));
				up = up ? up : node->GetObjectByName(RE::BSFixedString("Bow_UpBone1"));
			}
			const auto blade = rig::Unit({ a_pose.blade[3], a_pose.blade[4], a_pose.blade[5] });
			const bool left = (a_pose.flags & (1u << static_cast<std::uint32_t>(proto::PoseFlag::BowLeft))) != 0;
			const auto limb = mid && up ? UnitOf(node->world.rotate.Transpose() * (up->world.translate - mid->world.translate)) : std::nullopt;
			if (!node || !node->parent || !grip || !limb || !blade || left) {
				Release(node, g.bowMoved);
				return;
			}
			const auto rest = Begin(node, g.bowMoved);
			const auto now = UnitOf(rest.rotate * RE::NiPoint3((*limb)[0], (*limb)[1], (*limb)[2]));
			const auto want = UnitOf(ErWorldDir({ -(*blade)[0], -(*blade)[1], -(*blade)[2] }, a_rootRot, a_yaw));
			const auto arc = rig::RotationArc(now.value_or(*limb), want.value_or(*limb));
			if (g.applied % 120 == 0) {
				SKSE::log::info("[pose] bow carried in the right hand: limb (node space) {} turned {:.0f} deg", Fmt(*limb), rig::Angle(arc) * 57.2958f);
			}
			MoveTo(node, g.bowMoved, ToNi(arc) * rest.rotate, grip->world.translate);
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

	void Apply(RE::PlayerCharacter* a_player, const std::optional<proto::PoseState>& a_pose, const std::optional<proto::PoseBind>& a_bind,
		float a_facing, std::uint64_t a_frame)
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
		if (a_bind && a_bind->bone_count == kBones && a_bind->frame != g.rig.bindFrame && Finite(a_bind->dir, std::size(a_bind->dir))) {
			Refit(*a_bind);
		}
		const auto* camera = RE::PlayerCamera::GetSingleton();
		const bool firstPerson = camera && camera->IsInFirstPerson();
		// The ER writer already rejects non-finite poses; a mismatched DLL must still never write NaN positions into the skeleton.
		const bool valid = a_pose && a_pose->bone_count == kBones && Finite(a_pose->pelvis_offset, 3) && Finite(a_pose->rot, std::size(a_pose->rot));
		const bool active = valid && (a_pose->flags & kActive) && !firstPerson && g.rig.bindFrame != 0;
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
			Release(g.rig.weapon, g.weaponMoved);
			Release(g.rig.shield, g.bowMoved);
			return;
		}
		const auto& pose = *g.last;
		const auto& r = g.rig;

		const auto& rootRot = root->world.rotate;
		// Yaw from the root's actual heading (model +Y → world (sin h, cos h)), not GetAngleZ(): the two can differ for a frame after
		// SetHeading. Heading grows when turning right = negative rotation about +Z.
		const float rootHeading = std::atan2(rootRot.entry[0][1], rootRot.entry[1][1]);
		const auto yaw = rig::AxisAngle({ 0.0f, 0.0f, 1.0f }, -std::remainder(g.facing - rootHeading, 2.0f * 3.14159265f));
		std::array<RE::NiTransform, kBones> before;  // the animation's world transforms, for Carry
		for (std::size_t k = 0; k < kBones; ++k) {
			before[k] = r.nodes[k]->world;
		}
		Done done;
		// COM turns with the yaw too. Its rotation comes from Skyrim's animation, which faces the actor's heading, and it places the spine's
		// root relative to the pelvis: left alone, a body facing away from the actor's heading had its spine root beside or in front of the
		// hips ([body] probe 2026-10-05: lean 4-11 deg more than ER's, worst running back toward the camera).
		{
			const auto& above = r.mover->parent->world.rotate;
			const auto target = rootRot * ToNi(rig::Mul(yaw, ToQuat(rootRot.Transpose() * r.mover->world.rotate)));
			r.mover->local.rotate = ToNi(rig::Slerp(ToQuat(r.mover->local.rotate), ToQuat(above.Transpose() * target), g.weight));
			done.Set(r.mover, above * r.mover->local.rotate);
		}
		for (std::size_t k = 0; k < kBones; ++k) {
			auto* node = r.nodes[k];
			const auto parentWorld = node->parent ? WorldRot(node->parent, done) : RE::NiMatrix3{};
			if (const auto delta = rig::Normalize({ pose.rot[k * 4], pose.rot[k * 4 + 1], pose.rot[k * 4 + 2], pose.rot[k * 4 + 3] })) {
				const auto d = rig::Mul(rig::Conj(r.conj[k]), rig::Mul(*delta, r.conj[k]));  // identity conj = delta
				const auto world = rootRot * ToNi(rig::Mul(yaw, rig::Mul(rig::Mul(d, r.fit[k]), r.bind[k])));
				const auto target = ToQuat(parentWorld.Transpose() * world);
				node->local.rotate = ToNi(rig::Slerp(ToQuat(node->local.rotate), target, g.weight));
			}
			done.Set(node, parentWorld * node->local.rotate);
		}
		for (const auto& h : r.helpers) {
			h.node->local.rotate = ToNi(rig::Slerp(ToQuat(h.node->local.rotate), h.bindLocal, g.weight));
		}
		// Pelvis position: bind + ER's offset (metres → units, scaled to this body), turned by the same yaw. COM (the pelvis's parent,
		// also the spine's) is moved by the difference, so hips and torso drop together. The pelvis's world position isn't updated yet:
		// it's predicted from COM's new rotation (set above); the rotations above are world targets, so the move keeps them valid.
		{
			const float s = coords::kSkyrimUnitsPerM * r.heightRatio;
			const rig::Vec3 model = rig::Rotate(yaw, { r.pelvisBind[0] + pose.pelvis_offset[0] * s, r.pelvisBind[1] + pose.pelvis_offset[1] * s,
														 r.pelvisBind[2] + pose.pelvis_offset[2] * s });
			const auto target = root->world.translate + rootRot * RE::NiPoint3(model[0], model[1], model[2]) * root->world.scale;
			const auto& above = r.mover->parent->world;
			const auto pelvisNow = r.mover->world.translate + *done.Find(r.mover) * r.nodes[0]->local.translate * r.mover->world.scale;
			const auto shift = above.rotate.Transpose() * (target - pelvisNow) / above.scale;
			r.mover->local.translate = r.mover->local.translate + shift * g.weight;
		}
		RE::NiUpdateData update{};
		r.mover->UpdateDownwardPass(update, 0);
		AimWeapon(pose, rootRot, yaw);
		CarryBow(pose, rootRot, yaw);
		Carry(root, before);
		if (g.weight >= 1.0f) {
			MeasureBody(a_player, root);
		} else {
			body.last.reset();
		}
		// Check (first frames of each stretch, then every 300): the posed segment (bone → end) in model space vs ER's, after the yaw
		// and delta, for every bone with a segment. Big errors mean a wrong bind, fit or basis; the numbers say which bone.
		if (g.weight >= 1.0f && (g.applied == 10 || g.applied % 300 == 299)) {
			std::string line;
			for (std::size_t k = 0; k < kBones; ++k) {
				const auto want = rig::Unit(r.erDir[k]);
				if (!r.end[k] || !kLimb[k] || !want) {
					continue;
				}
				const auto d = rootRot.Transpose() * (r.end[k]->world.translate - r.nodes[k]->world.translate);
				const auto got = rig::Unit({ d.x, d.y, d.z });
				const rig::Quat delta{ pose.rot[k * 4], pose.rot[k * 4 + 1], pose.rot[k * 4 + 2], pose.rot[k * 4 + 3] };
				const auto posed = rig::Unit(rig::Rotate(rig::Mul(yaw, delta), *want));
				if (got && posed) {
					line += std::format(" {}={:.0f}", k, std::acos(std::clamp(rig::Dot(*got, *posed), -1.0f, 1.0f)) * 57.2958f);
				}
			}
			SKSE::log::info("[pose] check: limb segment error deg (PoseBone order):{} frame={}", line, a_frame);
		}
		++g.applied;
		g.maxUs = std::max(g.maxUs, std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count());
	}
}
