#include "TrueScopes/ScopeIdent.h"

#include "Settings/Settings.h"

namespace TrueScopes::ScopeIdent
{
	namespace
	{
		// --- engine entry points (RVAs, Fallout4VR.exe 1.2.72) -------------------
		constexpr std::uintptr_t kGetCurrentWeapon = 0xe50da0;   // Actor::GetCurrentWeapon(actor, out[2], equipIndex)
		constexpr std::uintptr_t kGetIronSightFOV = 0x332b80;    // TESObjectWEAP::GetIronSightFOV(weapon, instanceData) -> fovMult
		constexpr std::uintptr_t kGetZoomFOV = 0x332cb0;         // (weapon, baseFovDeg, instanceData) -> zoomed FOV in degrees
		constexpr std::uintptr_t kGetWeaponBone = 0xec5230;      // AIProcess::GetWeaponBone(process, actor, biped, equipIndex)
		constexpr std::uintptr_t kEquipIndexGlobal = 0x59444c8;  // u32; RIP-decoded from 0x140efab3e (see the header note)

		// --- object layout (read out of the VR binary's own code) ---------------
		constexpr std::uintptr_t kProcessInActor = 0x300;    // AIProcess* (FUN_140ef5be0: param_1[0x60])
		constexpr std::uintptr_t kGetBipedVFunc = 0x508;     // Actor::GetBiped(bool) -> BipedAnim**
		constexpr std::uintptr_t kNameInNiObjectNET = 0x10;  // BSFixedString (NiAVObject::GetObjectByName compares [+0x10])
		constexpr std::uintptr_t kChildrenInNiNode = 0x168;  // NiAVObject** (NiNode::GetObjectByName)
		constexpr std::uintptr_t kChildCountInNiNode = 0x172;  // u16
		constexpr std::uintptr_t kFormIDInTESForm = 0x14;
		// NiNode::GetObjectByName's own address. A node's vtable slot 0x188 holding
		// this exact pointer is how a child is known to BE an NiNode and therefore
		// safe to descend into. Anything else (BSTriShape, particle systems) is
		// visited for its name and not recursed -- reading +0x168 off a non-node
		// would be reading whatever field happens to live there.
		constexpr std::uintptr_t kNiNodeGetObjectByName = 0x1c18500;
		constexpr std::uintptr_t kGetObjectByNameVFunc = 0x188;

		// NiAVObject spatial layout (see the ShapeGeom comment in the header).
		constexpr std::uintptr_t kWorldTransform = 0x70;
		constexpr std::uintptr_t kWorldTranslate = 0xa0;
		constexpr std::uintptr_t kWorldScale = 0xac;
		constexpr std::uintptr_t kWorldBound = 0xb0;  // {NiPoint3 centre, float radius}
		constexpr std::size_t    kMatrixRowStride = 4;

		using GetCurrentWeapon_t = void (*)(std::uintptr_t, void*, std::uint32_t);
		using GetIronSightFOV_t = float (*)(std::uintptr_t, std::uintptr_t);
		using GetZoomFOV_t = float (*)(std::uintptr_t, float, std::uintptr_t);
		using GetWeaponBone_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, void*, std::uint32_t);

		template <class T>
		[[nodiscard]] T Fn(std::uintptr_t a_rva)
		{
			return reinterpret_cast<T>(REL::Module::get().base() + a_rva);
		}

		// Ocular radii measured from the shipped first-person meshes by
		// tools/scope-census.py (investigation repo). ONE of these is a real clear
		// aperture -- the hunting rifle, the only shipped scope with a distinct
		// glass material. The other 25 are measured on the tube body, which reads
		// about 15% high (hunting rifle: body 1.454 vs glass 1.267), so they are
		// starting points, not truth. That is exactly why they are overridable.
		//
		// "LaserScope" is the one ambiguous key: the laser rifle scope (1.175) and
		// the laser pistol recon scope (2.185) ship the same root node name. The
		// smaller value is used, since over-shrinking the widget shows the tube
		// around it while over-growing it spills the picture outside the lens.
		constexpr TableEntry kTable[] = {
			{ "MachineGunScope", 0.759f },
			{ "GaussRifleScope", 1.028f },
			{ "MissileLauncherScope", 1.105f },
			{ "LaserScope", 1.175f },
			{ "HuntingScope", 1.267f },
			{ "44MagScope", 1.290f },
			{ "HandMadeScope", 1.477f },
			{ "Box121", 1.592f },  // MachineGunScopeF -- the mesh really does name its root Box121
			{ "PipeReflex", 1.597f },
			{ "LaserScope002", 1.640f },
			{ "RailwayRifleScope", 1.864f },
			{ "FatmanScope", 2.159f },
			{ "44MagReconScope", 2.184f },
			{ "Pistol_scope_Railway", 2.184f },
			{ "GaussRifleReconScope", 2.184f },
			{ "Pistol_scope", 2.184f },
			{ "PlasmaScope002", 2.185f },
			{ "InstituteReconScope", 2.288f },
			{ "ReconScopeRifle", 2.288f },
			{ "AssaultRifleScope", 2.290f },
			{ "Scope", 3.058f },  // Laser_Gun_Set_10
			{ "MGThermalScope", 3.539f },
			{ "Scope01", 4.366f },  // alien blaster
			{ "PlasmaScope", 4.564f },
		};

		// Walk bounds. Generous relative to a weapon (the hunting rifle is 50 nodes,
		// 6 deep) but still bounded, so a corrupt child count or a cyclic graph costs
		// a fixed number of reads instead of hanging the render thread.
		constexpr int          kMaxDepth = 16;
		constexpr std::uint32_t kMaxNodes = 2048;

		std::atomic_bool g_request{ true };  // probe once as soon as a render happens
		std::mutex       g_lock;
		Info             g_info;

		void CopyName(char (&a_dst)[kNameLen], const char* a_src)
		{
			a_dst[0] = '\0';
			if (!a_src) {
				return;
			}
			std::size_t i = 0;
			for (; i + 1 < kNameLen && a_src[i]; ++i) {
				a_dst[i] = a_src[i];
			}
			a_dst[i] = '\0';
		}

		// A BSFixedString field holds a BSStringPool::Entry*, NOT a char*. Measured
		// live 2026-08-10 against an OMOD's model field:
		//     entry+0x00  next/pool pointer
		//     entry+0x10  u32 length          (read 43)
		//     entry+0x18  the characters      ("Weapons\ReconScope\...nif", 43 long)
		// v0.2.85 read the field as a char* and so returned the entry header as
		// text -- garbage for every node name. Validate rather than trust: the
		// declared length must agree with where the terminator actually sits, and
		// every character must be printable. A field that fails is reported as no
		// name, which is a visible miss; a field that "succeeds" wrongly is a
		// plausible-looking lie, and this project has been bitten by those.
		constexpr std::uintptr_t kStringPoolLength = 0x10;
		constexpr std::uintptr_t kStringPoolChars = 0x18;

		[[nodiscard]] bool ReadFixedString(std::uintptr_t a_field, char (&a_out)[kNameLen])
		{
			a_out[0] = '\0';
			const auto entry = *reinterpret_cast<std::uintptr_t*>(a_field);
			if (entry < 0x10000) {
				return false;
			}
			const auto len = *reinterpret_cast<std::uint32_t*>(entry + kStringPoolLength);
			if (len == 0 || len > 512) {
				return false;
			}
			const auto* chars = reinterpret_cast<const char*>(entry + kStringPoolChars);
			if (chars[len] != '\0') {
				return false;  // the length field and the terminator disagree
			}
			for (std::uint32_t i = 0; i < len; ++i) {
				const auto c = static_cast<unsigned char>(chars[i]);
				if (c < 0x20 || c > 0x7e) {
					return false;
				}
			}
			CopyName(a_out, chars);
			return true;
		}

		[[nodiscard]] bool NodeName(std::uintptr_t a_node, char (&a_out)[kNameLen])
		{
			return ReadFixedString(a_node + kNameInNiObjectNET, a_out);
		}

		[[nodiscard]] bool IsNode(std::uintptr_t a_obj)
		{
			const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_obj);
			if (!vtbl) {
				return false;
			}
			const auto slot = *reinterpret_cast<std::uintptr_t*>(vtbl + kGetObjectByNameVFunc);
			return slot == REL::Module::get().base() + kNiNodeGetObjectByName;
		}

		// Depth-first, breadth-capped. A weapon's 3D is small (tens of nodes), but
		// a bounded walk means a corrupt count or a cyclic graph costs a bounded
		// number of reads instead of hanging the render thread.
		// v0.2.89: the attach point, and why identification hangs off it.
		//
		// The first complete walk (hunting rifle, long scope) ended:
		//     ... P-Barrel, HuntingRifleBarrel:0, ProjectileNode,
		//         P-Scope, HuntingScope:0, HuntingScope:1, HuntingScope:2
		// Two facts in that tail:
		//
		// 1. The scope hangs off the receiver's "P-Scope" connect point. Matching
		//    table keys against P-Scope's SUBTREE instead of the whole weapon is
		//    what makes a key like "Scope" safe -- searching all 50 nodes would
		//    happily match some unrelated receiver or sight node and then fit the
		//    lens to it with complete confidence.
		//
		// 2. The mod's NIF ROOT NODE IS NOT INSTANTIATED. The mesh's root is named
		//    "HuntingScope"; at runtime its SHAPES are attached directly, named
		//    "HuntingScope:0/:1/:2". So the runtime name is the offline root name
		//    plus a ":N" suffix, and the v0.2.85 table -- keyed on bare root names
		//    measured from the meshes -- could never have matched anything. Strip
		//    at the colon and the offline census keys work unchanged.
		[[nodiscard]] bool IsScopeAttachPoint(const char* a_name)
		{
			return a_name && _stricmp(a_name, "P-Scope") == 0;
		}

		// "HuntingScope:2" -> "HuntingScope". Shapes from one mesh share the stem,
		// so all three of the hunting rifle's shapes resolve to the same key.
		void StripShapeSuffix(char (&a_name)[kNameLen])
		{
			if (auto* colon = std::strchr(a_name, ':'); colon) {
				*colon = '\0';
			}
		}

		// --- geometry capture (v0.2.92) -----------------------------------------

		void ReadGeom(std::uintptr_t a_node, const char* a_name, ShapeGeom& a_out)
		{
			CopyName(a_out.name, a_name);
			const auto* w = reinterpret_cast<const float*>(a_node + kWorldTranslate);
			a_out.world[0] = w[0];
			a_out.world[1] = w[1];
			a_out.world[2] = w[2];
			a_out.scale = *reinterpret_cast<const float*>(a_node + kWorldScale);
			const auto* m = reinterpret_cast<const float*>(a_node + kWorldTransform);
			for (std::size_t r = 0; r < 3; ++r) {
				for (std::size_t c = 0; c < 3; ++c) {
					a_out.rot[r * 3 + c] = m[r * kMatrixRowStride + c];
				}
			}
			const auto* b = reinterpret_cast<const float*>(a_node + kWorldBound);
			a_out.boundCenter[0] = b[0];
			a_out.boundCenter[1] = b[1];
			a_out.boundCenter[2] = b[2];
			a_out.boundRadius = b[3];
		}

		// A bound is only usable if it is finite and has real extent. An unbuilt or
		// never-updated bound reads as zero radius, and a zero-radius sphere at the
		// origin would drag the union to (0,0,0) — placement would then aim the disc
		// at the world origin with total confidence. Refuse instead.
		[[nodiscard]] bool BoundUsable(const ShapeGeom& a_g)
		{
			if (!std::isfinite(a_g.boundRadius) || a_g.boundRadius <= 0.001f || a_g.boundRadius > 1.0e5f) {
				return false;
			}
			for (const float v : a_g.boundCenter) {
				if (!std::isfinite(v)) {
					return false;
				}
			}
			return true;
		}

		// Standard merge of two bounding spheres, in place.
		void MergeBound(float (&a_c)[3], float& a_r, const float (&a_c2)[3], float a_r2)
		{
			const float dx = a_c2[0] - a_c[0];
			const float dy = a_c2[1] - a_c[1];
			const float dz = a_c2[2] - a_c[2];
			const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (d + a_r2 <= a_r) {
				return;  // already contained
			}
			if (d + a_r <= a_r2) {
				a_c[0] = a_c2[0];
				a_c[1] = a_c2[1];
				a_c[2] = a_c2[2];
				a_r = a_r2;
				return;
			}
			const float nr = (d + a_r + a_r2) * 0.5f;
			const float t = (d > 1.0e-6f) ? (nr - a_r) / d : 0.0f;
			a_c[0] += dx * t;
			a_c[1] += dy * t;
			a_c[2] += dz * t;
			a_r = nr;
		}

		void Walk(std::uintptr_t a_node, Info& a_out, int a_depth, bool a_inScope)
		{
			if (!a_node) {
				return;
			}
			// Both caps count what they refuse. A cap that silently prunes turns
			// "the scope is not in this weapon's 3D" and "we stopped looking" into
			// the same output, which is how v0.2.85's name cap wasted a bench cycle.
			if (a_depth > kMaxDepth || a_out.nodesVisited >= kMaxNodes) {
				++a_out.clipped;
				return;
			}
			++a_out.nodesVisited;

			char name[kNameLen] = {};
			const bool named = NodeName(a_node, name);
			if (named) {
				if (a_out.nameCount < kMaxNames) {
					CopyName(a_out.names[a_out.nameCount], name);
					++a_out.nameCount;
				} else {
					++a_out.nameOverflow;
				}
			}

			// Everything below the attach point is the scope, including the point
			// itself (harmless: "P-Scope" is not a table key).
			const bool inScope = a_inScope || (named && IsScopeAttachPoint(name));

			// v0.2.92: record where it is, not just what it is called.
			if (named && IsScopeAttachPoint(name)) {
				a_out.pScopeNode = a_node;
				ReadGeom(a_node, name, a_out.pScope);
				a_out.haveGeom = true;
			} else if (a_inScope) {
				// Descendants only: the attach point itself is a transform, not
				// glass, and folding its bound into the union would inflate it.
				ShapeGeom g{};
				ReadGeom(a_node, named ? name : "(unnamed)", g);
				if (a_out.shapeCount < kMaxShapeGeom) {
					a_out.shapes[a_out.shapeCount++] = g;
				}
				if (BoundUsable(g)) {
					if (a_out.boundsSeen == 0) {
						a_out.unionCenter[0] = g.boundCenter[0];
						a_out.unionCenter[1] = g.boundCenter[1];
						a_out.unionCenter[2] = g.boundCenter[2];
						a_out.unionRadius = g.boundRadius;
					} else {
						MergeBound(a_out.unionCenter, a_out.unionRadius, g.boundCenter, g.boundRadius);
					}
					++a_out.boundsSeen;
				}
			}

			if (inScope && named && a_out.scopeNameCount < kMaxScopeNames) {
				char key[kNameLen] = {};
				CopyName(key, name);
				StripShapeSuffix(key);
				bool dupe = IsScopeAttachPoint(key);
				for (std::uint32_t i = 0; !dupe && i < a_out.scopeNameCount; ++i) {
					dupe = std::strcmp(a_out.scopeNames[i], key) == 0;
				}
				if (!dupe && key[0]) {
					CopyName(a_out.scopeNames[a_out.scopeNameCount], key);
					++a_out.scopeNameCount;
				}
			}

			if (!IsNode(a_node)) {
				return;
			}
			const auto children = *reinterpret_cast<std::uintptr_t*>(a_node + kChildrenInNiNode);
			const auto count = *reinterpret_cast<std::uint16_t*>(a_node + kChildCountInNiNode);
			if (!children || count > 256) {
				return;
			}
			for (std::uint16_t i = 0; i < count; ++i) {
				Walk(*reinterpret_cast<std::uintptr_t*>(children + i * 8ull), a_out, a_depth + 1, inScope);
			}
		}

		void ReleaseInstanceData(std::uintptr_t a_data)
		{
			if (!a_data) {
				return;
			}
			// TBO_InstanceData is intrusively refcounted at +8 and destroyed through
			// vtable slot 0 with the deleting flag -- the exact sequence every vanilla
			// caller of GetCurrentWeapon runs on the way out (e.g. FUN_140efaa60).
			auto* refs = reinterpret_cast<volatile long*>(a_data + 8);
			if (::InterlockedDecrement(refs) == 0) {
				const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_data);
				reinterpret_cast<void (*)(std::uintptr_t, int)>(*reinterpret_cast<std::uintptr_t*>(vtbl))(a_data, 1);
			}
		}

		// The body of the probe, POD-only so the SEH wrapper below is legal.
		void ProbeImpl(std::uintptr_t a_player, Info& a_out)
		{
			const auto base = REL::Module::get().base();
			const auto equipIndex = *reinterpret_cast<std::uint32_t*>(base + kEquipIndexGlobal);

			// --- the weapon and its zoom data ---
			std::uintptr_t inst[2] = { 0, 0 };  // { TESObjectWEAP*, InstanceData* }
			Fn<GetCurrentWeapon_t>(kGetCurrentWeapon)(a_player, inst, equipIndex);
			if (inst[0]) {
				a_out.weaponFormID = *reinterpret_cast<std::uint32_t*>(inst[0] + kFormIDInTESForm);
				a_out.fovMult = Fn<GetIronSightFOV_t>(kGetIronSightFOV)(inst[0], inst[1]);
				a_out.zoomFovAt90 = Fn<GetZoomFOV_t>(kGetZoomFOV)(inst[0], 90.0f, inst[1]);
			}
			ReleaseInstanceData(inst[1]);

			// --- the weapon's 3D ---
			const auto process = *reinterpret_cast<std::uintptr_t*>(a_player + kProcessInActor);
			if (!process) {
				return;
			}
			const auto vtbl = *reinterpret_cast<std::uintptr_t*>(a_player);
			const auto getBiped = *reinterpret_cast<void* (**)(std::uintptr_t, char)>(vtbl + kGetBipedVFunc);
			void*      biped = getBiped(a_player, 1);  // 1 = first person, as the vanilla caller passes
			if (!biped) {
				return;
			}
			const auto node = Fn<GetWeaponBone_t>(kGetWeaponBone)(process, a_player, biped, equipIndex);
			if (!node) {
				return;
			}
			a_out.weaponNode = node;
			NodeName(node, a_out.weaponNodeName);
			Walk(node, a_out, 0, false);
		}

		// SEH wrapper. Its own function because __try cannot coexist with objects
		// that need unwinding, and the caller holds a std::scoped_lock.
		bool ProbeGuarded(std::uintptr_t a_player, Info& a_out)
		{
			__try {
				ProbeImpl(a_player, a_out);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The aperture table lookup. First matching descendant wins; the walk order
		// is the scene graph's, so a scope attached deeper than some other named
		// node still resolves as long as no other node collides with a table key.
		void Resolve(Info& a_out)
		{
			a_out.aperture = static_cast<float>(*Settings::widgetApertureRadius);
			a_out.fromTable = false;
			a_out.matched[0] = '\0';
			if (!*Settings::perScopeAperture) {
				return;
			}
			// Only names from under P-Scope are candidates. Matching the whole
			// weapon would let a key like "Scope" hit an unrelated node and fit the
			// lens to it with complete confidence -- a wrong answer that looks like
			// a right one, which is the failure mode this project keeps meeting.
			// A TOML entry wins outright, and it may specify position, radius or
			// both -- an entry with only offsets means "the built-in radius is
			// fine, where you put it is not", which is exactly the defect the
			// 2026-08-10 screenshots showed.
			for (std::uint32_t i = 0; i < a_out.scopeNameCount; ++i) {
				const auto e = Settings::ScopeEntryFor(a_out.scopeNames[i]);
				const bool haveOffset = !std::isnan(e.offsetX) || !std::isnan(e.offsetY) || !std::isnan(e.offsetZ);
				if (e.aperture <= 0.0 && !haveOffset) {
					continue;
				}
				CopyName(a_out.matched, a_out.scopeNames[i]);
				a_out.offsetX = static_cast<float>(e.offsetX);
				a_out.offsetY = static_cast<float>(e.offsetY);
				a_out.offsetZ = static_cast<float>(e.offsetZ);
				if (e.aperture > 0.0) {
					a_out.aperture = static_cast<float>(e.aperture);
					a_out.fromTable = true;
					return;
				}
				break;  // offsets taken; fall through so the built-in radius still applies
			}
			for (std::uint32_t i = 0; i < a_out.scopeNameCount; ++i) {
				for (const auto& e : kTable) {
					if (std::strcmp(e.node, a_out.scopeNames[i]) == 0) {
						CopyName(a_out.matched, e.node);
						a_out.aperture = e.aperture;
						a_out.fromTable = true;
						return;
					}
				}
			}
		}

		void LogInfo(const Info& a_i)
		{
			logger::info(
				FMT_STRING("SCOPE IDENT: weapon={:08X} node={} fovMult={:.3f} (zoom FOV at 90 deg = {:.2f}) "
				           "aperture={:.3f} ({}) nodes={} names={}+{} dropped"),
				a_i.weaponFormID, a_i.weaponNodeName[0] ? a_i.weaponNodeName : "(none)", a_i.fovMult, a_i.zoomFovAt90,
				a_i.aperture, a_i.fromTable ? a_i.matched : "fallback: widgetApertureRadius",
				a_i.nodesVisited, a_i.nameCount, a_i.nameOverflow);

			if (a_i.haveGeom) {
				logger::info(FMT_STRING("SCOPE IDENT geom: P-Scope at ({:.2f},{:.2f},{:.2f}) bound r={:.2f}; "
				                        "{} shape(s), {} with bounds, union c=({:.2f},{:.2f},{:.2f}) r={:.2f}"),
					a_i.pScope.world[0], a_i.pScope.world[1], a_i.pScope.world[2], a_i.pScope.boundRadius,
					a_i.shapeCount, a_i.boundsSeen,
					a_i.unionCenter[0], a_i.unionCenter[1], a_i.unionCenter[2], a_i.unionRadius);
			}

			// A miss with a truncated name list is not evidence of anything -- the
			// scope may simply be in the part that was dropped. Say so, loudly,
			// rather than letting it read as "this scope is unknown". v0.2.85's cap
			// of 40 did exactly that on the very first live probe.
			if (a_i.nameOverflow) {
				logger::warn(FMT_STRING("SCOPE IDENT: name list TRUNCATED - {} node name(s) dropped. "
				                        "Raise kMaxNames; any 'no table entry' below is unreliable."),
					a_i.nameOverflow);
			}
			if (a_i.clipped) {
				logger::warn(FMT_STRING("SCOPE IDENT: walk CLIPPED - {} subtree(s) refused by the depth/node "
				                        "caps. Part of the weapon was never looked at."),
					a_i.clipped);
			}

			// When nothing matched, the names ARE the finding: they are what a user
			// (or the next session) needs in order to add a [Scopes] entry. Print
			// them rather than making someone attach a debugger to see them.
			if (!a_i.fromTable) {
				if (a_i.scopeNameCount) {
					std::string all;
					for (std::uint32_t i = 0; i < a_i.scopeNameCount; ++i) {
						if (i) {
							all += ", ";
						}
						all += a_i.scopeNames[i];
					}
					logger::info(FMT_STRING("SCOPE IDENT: no table entry. Add one under [Scopes] in "
					                        "TrueScopesVR.toml keyed by one of: {}"),
						all);
				} else {
					// No P-Scope subtree at all: either this weapon mounts its optic
					// somewhere else, or there is no optic on it. Distinguishable from
					// "found it, do not know it", and worth saying so.
					logger::info("SCOPE IDENT: no P-Scope attach point in this weapon's 3D — "
					             "nothing to key on, using the fallback aperture"sv);
				}
			}
		}
	}

	void Request()
	{
		g_request.store(true);
	}

	void RunIfRequested(std::uintptr_t a_player)
	{
		if (!a_player || !g_request.exchange(false)) {
			return;
		}
		{
			const std::scoped_lock lock(g_lock);
			if (g_info.faulted) {
				return;
			}
		}

		Info info;
		if (!ProbeGuarded(a_player, info)) {
			const std::scoped_lock lock(g_lock);
			g_info.faulted = true;
			logger::error("SCOPE IDENT: the weapon 3D walk faulted — per-scope fitting disabled for this session"sv);
			return;
		}

		info.probed = true;
		Resolve(info);
		{
			const std::scoped_lock lock(g_lock);
			g_info = info;
		}
		LogInfo(info);
	}

	float ApertureRadius()
	{
		const std::scoped_lock lock(g_lock);
		// Before the first probe, and whenever per-scope fitting is off or the walk
		// faulted, this is the pre-v0.2.85 behaviour exactly: one global setting.
		if (!g_info.probed || !*Settings::perScopeAperture) {
			return static_cast<float>(*Settings::widgetApertureRadius);
		}
		return g_info.aperture;
	}

	void WidgetOffsets(float& a_x, float& a_y, float& a_z)
	{
		a_x = static_cast<float>(*Settings::widgetOffsetX);
		a_y = static_cast<float>(*Settings::widgetOffsetY);
		a_z = static_cast<float>(*Settings::widgetOffsetZ);
		const std::scoped_lock lock(g_lock);
		if (!g_info.probed || !*Settings::perScopeAperture) {
			return;
		}
		// Per axis, not all-or-nothing: a scope that only needs its vertical
		// position corrected should not have to restate the other two.
		if (!std::isnan(g_info.offsetX)) {
			a_x = g_info.offsetX;
		}
		if (!std::isnan(g_info.offsetY)) {
			a_y = g_info.offsetY;
		}
		if (!std::isnan(g_info.offsetZ)) {
			a_z = g_info.offsetZ;
		}
	}

	float FovMult()
	{
		const std::scoped_lock lock(g_lock);
		return (g_info.probed && g_info.fovMult > 0.0f) ? g_info.fovMult : 1.0f;
	}

	bool ScopeBound(float (&a_center)[3], float& a_radius)
	{
		const std::scoped_lock lock(g_lock);
		if (!g_info.probed || !g_info.haveGeom || g_info.boundsSeen == 0 || g_info.unionRadius <= 0.001f) {
			return false;
		}
		a_center[0] = g_info.unionCenter[0];
		a_center[1] = g_info.unionCenter[1];
		a_center[2] = g_info.unionCenter[2];
		a_radius = g_info.unionRadius;
		return true;
	}

	Info Get()
	{
		const std::scoped_lock lock(g_lock);
		return g_info;
	}

	std::span<const TableEntry> Table()
	{
		return { kTable, std::size(kTable) };
	}
}
