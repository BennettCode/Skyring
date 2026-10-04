#include "bridge/Pose.h"

#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "bridge/Input.h"

// Step 2 of docs/POSE-PLAN.md: prove we can pose a bone of the player's third-person skeleton, and find the right moment to do it.
// Bone writes follow SkyCraft (skse/src/Game.cpp: local transform write + UpdateDownwardPass on the node); the "write as late as possible"
// idea is from GTA San AnSkateas (sa-plugin/src/main.cpp ApplyRig). Both are virtual calls: no new Address Library ids.
namespace sxer::pose
{
	namespace
	{
		constexpr std::uint32_t kKeyAfterUpdate = 0x41;  // F7: write after PlayerCharacter::Update (0xAD)
		constexpr float kTiltRadians = 0.785398f;       // 45°
		constexpr int kTraceFrames = 8;                 // traced frames after a key goes down
		const char* const kPelvis = "NPC Pelvis [Pelv]";

		struct State
		{
			RE::NiAVObject* root = nullptr;  // third-person 3D the cache below belongs to
			RE::NiAVObject* pelvis = nullptr;
			bool logged = false;
			bool tilt = false;  // F7 held
			int traceLeft = 0;
			std::uint64_t frame = 0;
			bool haveWrite = false;
			RE::NiMatrix3 base;     // the animation's pelvis rotation the last write started from
			RE::NiMatrix3 written;  // what we wrote
			std::string trace;
		};
		State g;

		bool Same(const RE::NiMatrix3& a, const RE::NiMatrix3& b)
		{
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					if (std::fabs(a.entry[i][j] - b.entry[i][j]) > 1e-5f) {
						return false;
					}
				}
			}
			return true;
		}

		std::string Fmt(const RE::NiTransform& t)
		{
			const auto& m = t.rotate.entry;
			return std::format("t=({:.2f},{:.2f},{:.2f}) s={:.2f} r=[{:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f}]",
				t.translate.x, t.translate.y, t.translate.z, t.scale, m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
		}

		// Logs `items` as several [pose] lines of at most ~30 entries.
		void LogChunks(const std::string& a_title, const std::vector<std::string>& a_items)
		{
			for (std::size_t i = 0; i < a_items.size(); i += 30) {
				std::string line;
				for (std::size_t j = i; j < std::min(a_items.size(), i + 30); ++j) {
					line += a_items[j] + "; ";
				}
				SKSE::log::info("[pose] {} {}-{}: {}", a_title, i, std::min(a_items.size(), i + 30) - 1, line);
			}
		}

		void Walk(RE::NiAVObject* a_obj, const std::string& a_parent, std::vector<std::string>& a_nodes, std::vector<RE::BSGeometry*>& a_geoms)
		{
			if (!a_obj || a_nodes.size() > 600) {
				return;
			}
			const std::string name = a_obj->name.c_str();
			if (auto* geom = a_obj->AsGeometry()) {
				a_geoms.push_back(geom);
				return;
			}
			a_nodes.push_back(name + "<" + a_parent);
			if (auto* node = a_obj->AsNode()) {
				for (auto& child : node->GetChildren()) {
					Walk(child.get(), name, a_nodes, a_geoms);
				}
			}
		}

		// Once per 3D: bone tree, pelvis local transform, and every skinned geometry's bones (+ the pelvis inverse bind, skinToBone).
		void LogSkeleton(RE::NiAVObject* a_root)
		{
			std::vector<std::string> nodes;
			std::vector<RE::BSGeometry*> geoms;
			Walk(a_root, "-", nodes, geoms);
			SKSE::log::info("[pose] third-person 3D '{}': {} nodes, {} geometries", a_root->name.c_str(), nodes.size(), geoms.size());
			LogChunks("nodes (name<parent)", nodes);
			if (g.pelvis) {
				SKSE::log::info("[pose] {} local {} world {}", kPelvis, Fmt(g.pelvis->local), Fmt(g.pelvis->world));
			} else {
				SKSE::log::warn("[pose] '{}' not found", kPelvis);
			}
			for (auto* geom : geoms) {
				auto* skin = geom->GetGeometryRuntimeData().skinInstance.get();
				auto* data = skin ? skin->skinData.get() : nullptr;
				if (!data || !skin->bones) {
					SKSE::log::info("[pose] geometry '{}' (not skinned)", geom->name.c_str());
					continue;
				}
				std::vector<std::string> bones;
				int pelvisIndex = -1;
				for (std::uint32_t i = 0; i < data->bones; ++i) {
					auto* bone = skin->bones[i];
					bones.emplace_back(bone ? bone->name.c_str() : "?");
					if (bone == g.pelvis) {
						pelvisIndex = static_cast<int>(i);
					}
				}
				SKSE::log::info("[pose] geometry '{}' skinned: {} bones, pelvis index {}", geom->name.c_str(), data->bones, pelvisIndex);
				LogChunks(std::format("  '{}' bones", geom->name.c_str()), bones);
				if (pelvisIndex >= 0 && data->boneData) {
					SKSE::log::info("[pose]   pelvis skinToBone {}", Fmt(data->boneData[pelvisIndex].skinToBone));
				}
			}
		}

		// Finds the pelvis of the current third-person 3D (re-done when the 3D is rebuilt, e.g. after an equipment change or load).
		RE::NiAVObject* Pelvis(RE::PlayerCharacter* a_player)
		{
			auto* root = a_player->Get3D(false);
			if (root != g.root) {
				g.root = root;
				g.pelvis = root ? root->GetObjectByName(RE::BSFixedString(kPelvis)) : nullptr;
				g.haveWrite = false;
				if (root && !g.logged) {
					g.logged = true;
					LogSkeleton(root);
				}
			}
			return g.pelvis;
		}

		void Mark(const char* a_what)
		{
			if (g.traceLeft > 0 && g.haveWrite && g.pelvis) {
				g.trace += std::format(" {}:{}", a_what, Same(g.pelvis->local.rotate, g.written) ? "ours" : "anim");
			}
		}

		void Write(const char* a_where)
		{
			auto* pelvis = g.pelvis;
			// The animation's value, unless our own write survived from last frame (then keep tilting from the same base).
			const bool ours = g.haveWrite && Same(pelvis->local.rotate, g.written);
			if (!ours) {
				g.base = pelvis->local.rotate;
			}
			RE::NiMatrix3 tilt;
			const float c = std::cos(kTiltRadians), s = std::sin(kTiltRadians);
			tilt.entry[1][1] = c;
			tilt.entry[1][2] = -s;
			tilt.entry[2][1] = s;
			tilt.entry[2][2] = c;
			pelvis->local.rotate = g.base * tilt;
			RE::NiUpdateData update{};
			pelvis->UpdateDownwardPass(update, 0);
			g.written = pelvis->local.rotate;
			g.haveWrite = true;
			if (g.traceLeft > 0) {
				g.trace += std::format(" WRITE@{}", a_where);
			}
		}
	}

	void BeforePlayerUpdate(RE::PlayerCharacter* a_player)
	{
		if (!Pelvis(a_player)) {
			return;
		}
		Mark("preUpd");
	}

	void AfterPlayerUpdate(RE::PlayerCharacter* a_player)
	{
		++g.frame;
		auto* pelvis = Pelvis(a_player);
		const bool tilt = input::KeyHeld(kKeyAfterUpdate);
		if (tilt != g.tilt) {
			if (!g.trace.empty()) {
				SKSE::log::info("[pose] trace:{}", g.trace);
				g.trace.clear();
			}
			SKSE::log::info("[pose] F7 tilt {} frame={}", tilt ? "on" : "off", g.frame);
			g.tilt = tilt;
			g.traceLeft = tilt ? kTraceFrames : 0;
			g.haveWrite = false;
		}
		if (g.traceLeft > 0) {
			if (!g.trace.empty()) {
				SKSE::log::info("[pose] trace:{}", g.trace);
			}
			g.trace = std::format("f{}", g.frame);
			--g.traceLeft;
		}
		if (pelvis) {
			Mark("postUpd");
			if (g.tilt) {
				Write("postUpd");
			}
		}
	}
}
