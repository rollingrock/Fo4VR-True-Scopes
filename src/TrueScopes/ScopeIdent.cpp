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

		// --- the attached-mod chain (v0.2.101) ----------------------------------
		// Read out of the VR binary, not ported from CommonLibF4: QueueFiles
		// (0x140eda410) runs exactly these steps in this order to find an item's
		// object mods, and 0x35 is the kObjectInstance extra-data type as that
		// function passes it. The flatrim header agrees on every offset below,
		// which is a cross-check, not the source.
		constexpr std::uintptr_t kGetInventoryItem = 0x3e6270;      // TESObjectREFR::GetInventoryItem(refr, boundObj)
		constexpr std::uintptr_t kGetExtraDataAt = 0x1aeeb0;        // BGSInventoryItem::GetExtraDataAt(item, stackIdx)
		constexpr std::uintptr_t kGetExtraData = 0x442f0;           // ExtraDataList::GetExtraData(list, type)
		constexpr std::uintptr_t kGetExtraInstanceData = 0x8b040;   // ExtraDataList::GetInstanceData(list) -- no incref
		constexpr std::uintptr_t kGetModFromID = 0x3d650;           // BGSObjectInstanceExtra::GetModFromID(u32)
		constexpr std::uint8_t   kExtraObjectInstance = 0x35;       // EXTRA_DATA_TYPE::kObjectInstance

		// BGSMod::Attachment::Mod. TESModel is the BGSModelMaterialSwap base at
		// +0x48, so its BSFixedString model sits at +0x50 (TESModel+0x08).
		// INDEPENDENTLY CROSS-CHECKED: DevBench's OMOD string scan has always
		// reported the texture-ID block at +0x58, and TESModel::textures is at
		// TESModel+0x10 -- 0x48 + 0x10 = 0x58. Two unrelated observations, one
		// layout, so the base offset is not a guess.
		constexpr std::uintptr_t kModelPathInMod = 0x50;
		constexpr std::uintptr_t kAttachPointInMod = 0xc0;  // u16 keyword value

		// BGSObjectInstanceExtra::values -- a BSTDataBuffer<1>* holding the
		// attached-mod array. See ReadAttachedMods for the buffer format.
		constexpr std::uintptr_t kValuesInObjectInstanceExtra = 0x18;
		// A weapon in the player's inventory can exist as several stacks with
		// different mods. Walking a few is cheap; walking unboundedly is not.
		constexpr int kMaxStacks = 16;

		using GetInventoryItem_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);
		using GetExtraDataAt_t = std::uintptr_t (*)(std::uintptr_t, std::int32_t);
		using GetExtraData_t = std::uintptr_t (*)(std::uintptr_t, std::uint8_t);
		using GetExtraInstanceData_t = std::uintptr_t (*)(std::uintptr_t);
		using GetModFromID_t = std::uintptr_t (*)(std::uint32_t);

		using GetCurrentWeapon_t = void (*)(std::uintptr_t, void*, std::uint32_t);
		using GetIronSightFOV_t = float (*)(std::uintptr_t, std::uintptr_t);
		using GetZoomFOV_t = float (*)(std::uintptr_t, float, std::uintptr_t);
		using GetWeaponBone_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, void*, std::uint32_t);

		template <class T>
		[[nodiscard]] T Fn(std::uintptr_t a_rva)
		{
			return reinterpret_cast<T>(REL::Module::get().base() + a_rva);
		}

		// Per-scope ocular RADIUS and FACE POSITION, measured from the shipped
		// first-person meshes by tools/scope-census.py (investigation repo).
		//
		// Three kinds of row, and the "via" comment on each says which:
		//
		//   glass  (1)  the hunting rifle, the only shipped optic with a distinct
		//               glass shape -- a real clear aperture, and the one radius
		//               confirmed by eye in VR.
		//   screen (12) FLAT-QUAD optics: recon and pistol-recon scopes, both
		//               machine-gun scopes, the thermal, the assault and institute
		//               rifle scopes. These have no tube at all, and until v0.2.98
		//               the census measured their boxy HOUSING instead of the
		//               display -- 1.5x too wide on the recon scopes, 1.8x on the
		//               pistol ones, 2.3x on MachineGunScopeF, 5.2x on
		//               MGScopeThermal. Confirmed in VR: the recon scope's disc
		//               visibly overflowed its screen at 2.288 and sat correctly at
		//               the screen-derived value.
		//   body   (13) genuine tubes measured at the narrowest ocular
		//               cross-section. These read ~15% high (hunting rifle: body
		//               1.454 vs glass 1.267), so they are starting points, not
		//               truth -- which is why [Scopes] can override them.
		//
		// COLLIDING KEYS are resolved by SHAPE, not by order. Three meshes name
		// their root "LaserScope": the laser rifle's scope, the laser musket's, and
		// the laser pistol's recon screen (1.206). All three rows are present, and
		// Resolve() picks the one whose measured shape is actually in the weapon's
		// 3D -- evidence rather than ordering luck. A key match with no shape match
		// still supplies the aperture but no face, and says so in the log.
		//
		// The face is in the MEASURED SHAPE's own space, so it must be pushed
		// through that shape's live world transform — hence the shape name in the
		// row. Matching the exact shape also means a mesh whose shapes moved
		// between versions fails to resolve instead of placing the disc confidently
		// in the wrong spot.
		//
		// The face is in FILE COORDINATES -- measured from the mesh origin, which IS
		// the runtime node's origin. Established live 2026-08-11 by recovering that
		// origin empirically rather than assuming it: with the correct (transposed)
		// rotation, R^T * (runtime boundCentre - node world translate) reproduced
		// each shape's file-declared bound centre exactly.
		//
		// v0.2.95 briefly expressed these relative to the bounding-sphere centre, on
		// the theory that BSTriShape's bound-relative vertex storage moved the node
		// origin. That was WRONG and made the placement visibly worse. The real
		// fault was the rotation being applied transposed -- and because a rotation
		// used the wrong way round preserves LENGTH, the magnitude evidence that
		// appeared to support the bound-centre theory was equally well explained by
		// the transpose bug. Two different wrong stories, one symptom; only a
		// direction-preserving (vector, not magnitude) comparison told them apart.
		//
		// Generated by `python tools/scope-census.py --cpp`.
		constexpr TableEntry kTable[] = {
			// model path (primary key), node name (fallback key), aperture,
			// measured shape, ocular face (file coords), |face - bound centre|
		{ "weapons/machinegun/mgscopethermal.nif", "MGThermalScope", 0.685f, "Screen023:0", { 0.000f, 0.004f, 0.000f }, 0.000f },  // MGScopeThermal_1.nif via screen
		{ "weapons/machinegun/mgscope.nif", "MachineGunScope", 0.690f, "MachineGunScope:7", { -0.005f, -2.875f, 3.356f }, 0.001f },  // MGScope_1.nif via screen
		{ "weapons/machinegun/scopes/machinegunscopef.nif", "Box121", 0.690f, "Box121:7", { -0.005f, -2.875f, 3.356f }, 0.001f },  // MachineGunScopeF.nif via screen
		{ "weapons/gaussrifle/gaussriflescope.nif", "GaussRifleScope", 1.028f, "GaussRifleScope:0", { 0.022f, -3.260f, 1.500f }, 2.424f },  // GaussRifleScope_1.nif via body
		{ "weapons/missilelauncher/missilelauncherscope.nif", "MissileLauncherScope", 1.105f, "MissileLauncherScope:0", { -10.101f, -2.869f, 3.277f }, 5.961f },  // MissileLauncherScope_1.nif via body
		{ "weapons/lasermusket/lasermusketlaserscope.nif", "LaserScope", 1.175f, "LaserScope:1", { -0.010f, -8.539f, 3.733f }, 6.430f },  // LaserMusketLaserScope.nif via body
		{ "weapons/laserweapons/laserscope.nif", "LaserScope", 1.175f, "LaserScope:0", { -0.007f, -8.539f, 3.389f }, 6.432f },  // LaserScope_1.nif via body
		{ "weapons/reconscope/pistol_scope_44mag.nif", "44MagReconScope", 1.206f, "Screen023:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_scope_44Mag.nif via screen
		{ "weapons/reconscope/pistol_scope_gauss.nif", "GaussRifleReconScope", 1.206f, "Screen024:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_Scope_Gauss.nif via screen
		{ "weapons/reconscope/pistol_scope_laser.nif", "LaserScope", 1.206f, "Screen021:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_scope_laser.nif via screen
		{ "weapons/reconscope/pistol_scope_plasma.nif", "PlasmaScope002", 1.206f, "Screen019:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_scope_Plasma.nif via screen
		{ "weapons/reconscope/pistol_scope_railway.nif", "Pistol_scope_Railway", 1.206f, "Screen018:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_scope_Railway.nif via screen
		{ "weapons/reconscope/recon_pistol_scope.nif", "Pistol_scope", 1.206f, "Screen020:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Recon_Pistol_scope.nif via screen
		{ "weapons/huntingrifle/huntingriflescope.nif", "HuntingScope", 1.267f, "HuntingScope:2", { 0.000f, -6.422f, 1.814f }, 11.043f },  // HuntingRifleScope_1.nif via glass
		{ "weapons/44/44scope.nif", "44MagScope", 1.290f, "44MagScope:0", { -0.002f, -8.539f, 2.118f }, 8.657f },  // 44Scope_1.nif via body
		{ "weapons/handmade/scopes/handmadescopezoom.nif", "HandMadeScope", 1.477f, "HandMadeScope:0", { 0.007f, -6.715f, 2.604f }, 8.078f },  // HandMadeScopeZoom_1.nif via body
		{ "weapons/reconscope/reconscoperifle.nif", "ReconScopeRifle", 1.484f, "Screen016:1", { 0.006f, -6.816f, 5.340f }, 0.002f },  // ReconScopeRifle.nif via screen
		{ "weapons/reconscope/rifle_scope_assault.nif", "AssaultRifleScope", 1.484f, "Screen017:1", { 0.006f, -6.816f, 5.340f }, 0.002f },  // Rifle_Scope_Assault.nif via screen
		{ "weapons/reconscope/rifle_scope_institute.nif", "InstituteReconScope", 1.484f, "Screen018:1", { 0.006f, -6.816f, 5.340f }, 0.002f },  // Rifle_Scope_Institute.nif via screen
		{ "weapons/handmade/scopes/handmadescope.nif", "PipeReflex", 1.597f, "PipeReflex:0", { 0.175f, -3.750f, 2.415f }, 2.883f },  // HandmadeScope_1.nif via body
		{ "weapons/laserweapons/laserscopenobracket.nif", "LaserScope002", 1.640f, "LaserScope002:0", { -0.498f, -2.957f, 2.852f }, 4.550f },  // LaserScopeNoBracket.nif via body
		{ "weapons/railwayrifle/railwayriflescope.nif", "RailwayRifleScope", 1.864f, "RailwayRifleScope:0", { 0.034f, -0.773f, 0.935f }, 4.857f },  // RailwayRifleScope_1.nif via body
		{ "weapons/fatman/fatmanscope.nif", "FatmanScope", 2.159f, "FatmanScope:0", { -2.223f, -6.680f, -7.689f }, 4.826f },  // FatmanScope_1.nif via body
		{ "weapons/laserweapons/institute/scope.nif", "Scope", 3.058f, "Scope:0", { 0.769f, -13.750f, 4.406f }, 4.782f },  // Scope_1.nif via body
		{ "weapons/alienblaster/scope01.nif", "Scope01", 4.366f, "Scope01:0", { 0.000f, -5.059f, 2.985f }, 6.633f },  // Scope01_1.nif via body
		{ "weapons/plasma/scope.nif", "PlasmaScope", 4.564f, "PlasmaScope:1", { 0.000f, -2.012f, 2.309f }, 6.117f },  // Scope_1.nif via body
		// --- DLC scopes (2026-08-23): measured from the DLCCoast/DLCNukaWorld BA2s
		// with the same tools/scope-census.py; the /omods sweep found 20 OMODs
		// pointing at these six paths. GammaRifleScope reuses the hunting rifle's
		// glass shape verbatim (r95 identical). 44MagScopeWestern ships NO _1
		// variant - the world model IS the first-person model (key unaffected:
		// the normalizer only strips a trailing "_1").
		{ "dlc03/weapons/harpoongun/harpoongunscope.nif", "Scope", 0.756f, "Scope:1", { -0.038f, -14.148f, 0.486f }, 9.695f },  // HarpoonGunScope_1.nif via body
		{ "dlc03/weapons/levergun/4570scope.nif", "4570Scope", 1.004f, "4570Scope:1", { -0.028f, -5.480f, 1.090f }, 7.684f },  // 4570Scope_1.nif via body
		{ "dlc04/weapons/raiderguns/scopelong.nif", "ScopeLong", 1.083f, "ScopeLong:0", { 0.005f, -19.609f, 1.828f }, 9.846f },  // ScopeLong_1.nif via body
		{ "dlc04/weapons/raiderguns/scopeshort.nif", "ScopeShort", 1.083f, "ScopeShort:0", { 0.005f, -19.609f, 1.828f }, 9.436f },  // ScopeShort_1.nif via body
		{ "dlc03/weapons/gammarifle/gammariflescope.nif", "GammaRifleScope", 1.267f, "HuntingScope:2", { 0.000f, -5.289f, 1.814f }, 7.767f },  // GammaRifleScope_1.nif via glass
		{ "dlc04/weapons/44western/44magscopewestern.nif", "44MagScopeWestern", 1.290f, "44MagScopeWestern:0", { -0.002f, -8.539f, 2.118f }, 8.657f },  // 44MagScopeWestern.nif via body
		};

		// Walk bounds. Generous relative to a weapon (the hunting rifle is 50 nodes,
		// 6 deep) but still bounded, so a corrupt child count or a cyclic graph costs
		// a fixed number of reads instead of hanging the render thread.
		constexpr int          kMaxDepth = 16;
		constexpr std::uint32_t kMaxNodes = 2048;

		std::atomic_bool g_request{ true };  // probe once as soon as a render happens
		std::mutex       g_lock;
		Info             g_info;

		// Templated on the buffer size since v0.2.101: node names are 64 bytes and
		// model paths are 128, and the compiler picking the wrong one is a silent
		// truncation that would turn a path key into a near-miss.
		template <std::size_t N>
		void CopyName(char (&a_dst)[N], const char* a_src)
		{
			a_dst[0] = '\0';
			if (!a_src) {
				return;
			}
			std::size_t i = 0;
			for (; i + 1 < N && a_src[i]; ++i) {
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

		template <std::size_t N>
		[[nodiscard]] bool ReadFixedString(std::uintptr_t a_field, char (&a_out)[N])
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
			// A string that does not fit is refused, not truncated: a truncated
			// model path is a key that silently matches nothing.
			if (len + 1 > N) {
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
			a_out.node = a_node;
			CopyName(a_out.name, a_name);
			const auto* w = reinterpret_cast<const float*>(a_node + kWorldTranslate);
			a_out.world[0] = w[0];
			a_out.world[1] = w[1];
			a_out.world[2] = w[2];
			a_out.scale = *reinterpret_cast<const float*>(a_node + kWorldScale);
			// TRANSPOSED ON READ (v0.2.96). The engine stores this matrix
			// column-major: the true mapping is world = T + s * (M_stored^T * v).
			//
			// Proven live 2026-08-11 from a single frame, so the weapon cannot move
			// between samples. Each scope shape's runtime worldBound centre is
			// displaced from its node by the file's declared bound centre c, and
			//     M_stored^T * c  reproduced that displacement to 0.001
			//     M_stored   * c  was wrong by ~5.8
			// on all three of the hunting rifle's shapes. Reading the transpose here
			// means rot[] is the real rotation, so every consumer -- and the
			// pScopeRot published by DevBench -- can use the textbook form.
			//
			// This was THE bug behind two rounds of misplaced widget: v0.2.93 aimed
			// the disc with M_stored and it landed ~5 units off, and v0.2.95 then
			// "explained" that with a bound-centre theory that was also wrong and
			// made it worse. A rotation applied the wrong way round preserves
			// LENGTH, which is why every magnitude-based check agreed with it.
			const auto* m = reinterpret_cast<const float*>(a_node + kWorldTransform);
			for (std::size_t r = 0; r < 3; ++r) {
				for (std::size_t c = 0; c < 3; ++c) {
					a_out.rot[r * 3 + c] = m[c * kMatrixRowStride + r];
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

		// --- attached object mods (v0.2.101) ------------------------------------

		// Every attached OMOD on the equipped weapon, with its model path. The
		// path is the only key that is unique per mesh, which is what makes a
		// modded optic distinguishable from the vanilla one whose root node name
		// it copied (see the header note).
		//
		// Declines with a REASON rather than an empty list. "This weapon has no
		// mods" and "the layout moved and we read nothing" are different facts and
		// have to look different, because the v0.2.85 table was dead for four
		// versions while looking exactly like a working one.
		void ReadAttachedMods(std::uintptr_t a_player, std::uintptr_t a_weapon,
			std::uintptr_t a_instanceData, Info& a_out)
		{
			const auto fail = [&a_out](const char* a_why) {
				CopyName(a_out.modsError, a_why);
			};
			if (!a_weapon) {
				fail("no equipped weapon");
				return;
			}
			const auto item = Fn<GetInventoryItem_t>(kGetInventoryItem)(a_player, a_weapon);
			if (!item) {
				fail("the equipped weapon is not in the inventory list");
				return;
			}

			// WHICH STACK. A base weapon can sit in the inventory several times
			// with different mods, so the stack is chosen by IDENTITY: the one
			// whose instance data IS the object Actor::GetCurrentWeapon just
			// returned. Taking stack 0 would happily describe a spare rifle in the
			// player's pack and there would be nothing in the output to say so.
			std::uintptr_t chosen = 0;
			std::uintptr_t firstList = 0;
			for (int s = 0; s < kMaxStacks; ++s) {
				const auto list = Fn<GetExtraDataAt_t>(kGetExtraDataAt)(item, s);
				if (!list) {
					break;  // walked off the end of the stack chain
				}
				if (!firstList) {
					firstList = list;
				}
				++a_out.stacksSeen;
				if (Fn<GetExtraInstanceData_t>(kGetExtraInstanceData)(list) == a_instanceData) {
					chosen = list;
					CopyName(a_out.stackPick, "instance");
					break;
				}
			}
			if (!chosen && a_out.stacksSeen == 1) {
				// The identity comparison assumes GetCurrentWeapon hands back the
				// same TBO_InstanceData object the inventory stack holds. That is
				// what the engine's own QueueFiles path implies, but it has not
				// been confirmed live. With exactly one stack the question is moot
				// -- there is no other stack it could be -- so take it and SAY SO,
				// rather than failing on an assumption that may simply be wrong.
				// If every probe reports "sole", the identity rule never fires and
				// the comparison needs revisiting.
				chosen = firstList;
				CopyName(a_out.stackPick, "sole");
			}
			if (!chosen) {
				// A weapon with no mods at all has no instance data, so this is
				// the ordinary outcome for a bare weapon -- not necessarily a bug.
				fail(a_instanceData ? "no inventory stack matches the equipped instance data"
									: "the equipped weapon has no instance data (no mods attached)");
				return;
			}

			const auto extra = Fn<GetExtraData_t>(kGetExtraData)(chosen, kExtraObjectInstance);
			if (!extra) {
				fail("no BGSObjectInstanceExtra on the equipped stack");
				return;
			}

			// BSTDataBuffer<1>: { char* buffer; u32 size; }. The block table lives
			// AT buffer+size (one 4-byte entry for N=1): size:24 in the low bits,
			// id:8 in the top byte, 0xFF meaning "no block". Block id 0 is the
			// BGSMod::ObjectIndexData array, 8 bytes each:
			//     +0 u32 objectID   +4 u8 index   +5 u8 rank   +6 u8 disabled
			// This is the decode ForEachModIndexData performs inline; it is
			// reproduced rather than called because every instantiation of it in
			// the binary has a different lambda welded into it.
			const auto values = *reinterpret_cast<std::uintptr_t*>(extra + kValuesInObjectInstanceExtra);
			if (!values) {
				fail("the mod list buffer is null");
				return;
			}
			const auto data = *reinterpret_cast<std::uintptr_t*>(values);
			const auto size = *reinterpret_cast<std::uint32_t*>(values + 8);
			if (!data || size == 0 || size > 0x100000) {
				fail("the mod list buffer looks wrong");
				return;
			}
			const auto block = *reinterpret_cast<std::uint32_t*>(data + size);
			const auto blockID = block >> 24;
			if (blockID == 0xFF) {
				fail("the mod list buffer holds no blocks");
				return;
			}
			if (blockID != 0) {
				// Block 0 is always first in practice; if it ever is not, say so
				// instead of walking whatever block happens to be there.
				fail("the first mod-list block is not the index-data block");
				return;
			}
			const auto bytes = block & 0xFFFFFFu;
			if (bytes > size || (bytes % 8) != 0) {
				fail("the mod-list block size is not a whole number of entries");
				return;
			}

			a_out.haveMods = true;
			const auto count = bytes / 8;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto entry = data + static_cast<std::uintptr_t>(i) * 8;
				if (*reinterpret_cast<std::uint8_t*>(entry + 6) != 0) {
					continue;  // disabled
				}
				const auto mod = Fn<GetModFromID_t>(kGetModFromID)(*reinterpret_cast<std::uint32_t*>(entry));
				if (!mod) {
					continue;  // the form is gone, or is not an OMOD
				}
				if (a_out.modCount >= kMaxMods) {
					++a_out.modOverflow;
					continue;
				}
				auto& m = a_out.mods[a_out.modCount];
				m.formID = *reinterpret_cast<std::uint32_t*>(mod + kFormIDInTESForm);
				m.attachPoint = *reinterpret_cast<std::uint16_t*>(mod + kAttachPointInMod);
				if (!ReadFixedString(mod + kModelPathInMod, m.path)) {
					// Plenty of OMODs are pure stat changes with no model. They
					// are still counted, so the list reflects the weapon.
					m.path[0] = '\0';
				}
				Settings::NormalizeScopeKeyInto(m.path, m.key, sizeof(m.key));
				++a_out.modCount;
			}
			if (a_out.modCount == 0) {
				fail("no enabled mods with readable forms");
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
			// BEFORE the release: the instance-data POINTER is what identifies the
			// equipped inventory stack, and the reference we hold is what keeps it
			// alive while we compare against it.
			ReadAttachedMods(a_player, inst[0], inst[1], a_out);
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

		// Is the shape this row was measured on actually in the weapon's 3D? That
		// decides whether the census FACE is usable -- the face is expressed in
		// that shape's own space, so without the shape there is nothing to push it
		// through and placement must fall back to the bound heuristic.
		[[nodiscard]] bool ShapePresent(const Info& a_i, const char* a_shape)
		{
			for (std::uint32_t s = 0; s < a_i.shapeCount; ++s) {
				if (std::strcmp(a_i.shapes[s].name, a_shape) == 0) {
					return true;
				}
			}
			return false;
		}

		void ApplyRow(Info& a_out, const TableEntry& a_e, const char* a_key, const char* a_by)
		{
			CopyName(a_out.matched, a_key);
			CopyName(a_out.matchedBy, a_by);
			a_out.aperture = a_e.aperture;
			a_out.fromTable = true;
			CopyName(a_out.faceShape, a_e.shape);
			if (ShapePresent(a_out, a_e.shape)) {
				a_out.face[0] = a_e.face[0];
				a_out.face[1] = a_e.face[1];
				a_out.face[2] = a_e.face[2];
				a_out.faceFromCentre = a_e.faceFromCentre;
				a_out.haveFace = true;
			}
			// faceShape is copied either way: when the shape is absent it is what
			// the warning in LogInfo names, so "no face" says WHICH shape is missing.
		}

		// The aperture table lookup, in strict order of evidence quality.
		void Resolve(Info& a_out)
		{
			a_out.aperture = static_cast<float>(*Settings::widgetApertureRadius);
			a_out.fromTable = false;
			a_out.matched[0] = '\0';
			a_out.matchedBy[0] = '\0';
			a_out.overrideKey[0] = '\0';
			if (!*Settings::perScopeAperture) {
				return;
			}

			// ---- 1. BY MODEL PATH (v0.2.101) --------------------------------
			// The only key that is unique per mesh. A modded optic that copied a
			// vanilla root node name has its own path, so this is the pass that
			// stops it being handed the vanilla numbers.
			//
			// Every attached mod is tried, not just a guessed "the scope one":
			// a weapon's receiver, barrel and muzzle mods simply are not in a
			// table of scopes, so the census itself does the selecting. That
			// also means we do not need to identify the scope attach-point slot
			// to get this right -- only to NAME an optic we have no row for.
			for (std::uint32_t m = 0; m < a_out.modCount && !a_out.fromTable; ++m) {
				if (!a_out.mods[m].key[0]) {
					continue;
				}
				for (const auto& e : kTable) {
					if (std::strcmp(e.path, a_out.mods[m].key) == 0) {
						ApplyRow(a_out, e, a_out.mods[m].key, "path");
						break;
					}
				}
			}

			// ---- 2. BY NODE NAME (pre-v0.2.101 behaviour) --------------------
			// Still needed, and not merely as belt-and-braces: an optic welded
			// into a weapon's base mesh is not an OMOD at all and has no path to
			// key on, and the mod chain can decline for the ordinary reasons in
			// Info::modsError. It is second because it is the weaker evidence.
			//
			// Only names from under P-Scope are candidates. Matching the whole
			// weapon would let a key like "Scope" hit an unrelated node and fit the
			// lens to it with complete confidence -- a wrong answer that looks like
			// a right one, which is the failure mode this project keeps meeting.
			//
			// Among rows sharing a node key, PREFER THE ONE WHOSE MEASURED SHAPE IS
			// ACTUALLY IN THE SCENE. Three shipped meshes call their root node
			// "LaserScope" -- the laser rifle scope (1.175), the laser musket's
			// (1.175) and the laser pistol's recon screen (1.206) -- and taking the
			// first key match would fit two of them with a radius measured off the
			// third. The shape name is what distinguishes them, and it is checked
			// against the live 3D, so the disambiguation is evidence rather than
			// ordering luck. A key match with no shape match still supplies the
			// aperture (better than the global fallback) but no face, and the
			// warning in LogInfo says placement has dropped to the heuristic.
			for (std::uint32_t i = 0; i < a_out.scopeNameCount && !a_out.fromTable; ++i) {
				const TableEntry* keyOnly = nullptr;
				for (const auto& e : kTable) {
					if (std::strcmp(e.node, a_out.scopeNames[i]) != 0) {
						continue;
					}
					if (!ShapePresent(a_out, e.shape)) {
						if (!keyOnly) {
							keyOnly = &e;
						}
						continue;
					}
					ApplyRow(a_out, e, e.node, "node");
					break;
				}
				if (!a_out.fromTable && keyOnly) {
					ApplyRow(a_out, *keyOnly, keyOnly->node, "node");
				}
			}

			// ---- 3. TOML overrides, applied ON TOP ---------------------------
			// An entry may carry a radius, a position, or both -- offsets alone
			// mean "the built-in radius is fine, where you put it is not", and an
			// aperture alone means the reverse. Neither disturbs the census face
			// resolved above.
			//
			// ORDER MATTERS, and v0.2.96 got it wrong. The TOML loop used to run
			// FIRST and RETURN as soon as it found an aperture, which skipped the
			// built-in table -- and the table is what resolves the census FACE. So
			// setting a per-scope aperture silently downgraded placement to the
			// bounding-sphere heuristic, moving the widget off the lens. Found in VR
			// on the recon scope: `method` flipped from "census" to "bound" the
			// moment an override was added, and nothing said so.
			//
			// Model paths are offered to the user BEFORE node names for the same
			// reason the table tries them first: an entry keyed on "HuntingScope"
			// retunes every mesh that copied that root name, and the user who wrote
			// it was thinking about one of them.
			const auto applyOverride = [&a_out](const char* a_key) {
				const auto e = Settings::ScopeEntryFor(a_key);
				const bool haveOffset = !std::isnan(e.offsetX) || !std::isnan(e.offsetY) || !std::isnan(e.offsetZ);
				if (e.aperture <= 0.0 && !haveOffset) {
					return false;
				}
				CopyName(a_out.overrideKey, a_key);
				a_out.offsetX = static_cast<float>(e.offsetX);
				a_out.offsetY = static_cast<float>(e.offsetY);
				a_out.offsetZ = static_cast<float>(e.offsetZ);
				if (e.aperture > 0.0) {
					a_out.aperture = static_cast<float>(e.aperture);
					a_out.fromTable = true;
				}
				return true;
			};
			for (std::uint32_t m = 0; m < a_out.modCount; ++m) {
				if (a_out.mods[m].key[0] && applyOverride(a_out.mods[m].key)) {
					return;
				}
			}
			for (std::uint32_t i = 0; i < a_out.scopeNameCount; ++i) {
				if (applyOverride(a_out.scopeNames[i])) {
					return;
				}
			}
		}

		// THE ONE CHECK THAT PROTECTS THE PATH KEY, run once at the first probe.
		//
		// The table's keys are produced by mesh_key() in tools/scope-census.py and
		// consumed by Settings::NormalizeScopeKeyInto here. Nothing in the build
		// connects those two, so if they ever disagree the lookup does not fail --
		// it simply never matches, quietly falls through to the node-name pass, and
		// looks exactly like a table that works. That is not hypothetical: the
		// v0.2.85 table was keyed on bare root names and COULD NOT MATCH ANYTHING,
		// for any scope, for four versions, and went unnoticed because the hunting
		// rifle's fallback and table values were both 1.267.
		//
		// A normalized key must be a fixed point of the normalizer, so
		// Normalize(row.path) == row.path is a direct test of agreement between the
		// two implementations, using the real data. Uniqueness is checked in the
		// same pass because the path key's entire justification is being unique.
		void VerifyTable()
		{
			std::uint32_t badKeys = 0;
			std::uint32_t dupes = 0;
			for (const auto& e : kTable) {
				char norm[kPathLen] = {};
				Settings::NormalizeScopeKeyInto(e.path, norm, sizeof(norm));
				if (std::strcmp(norm, e.path) != 0) {
					++badKeys;
					logger::error(FMT_STRING("SCOPE TABLE: key '{}' normalizes to '{}' — scope-census.py and "
					                         "NormalizeScopeKeyInto disagree, so this row can NEVER match."),
						e.path, norm);
				}
				for (const auto& o : kTable) {
					if (&o != &e && std::strcmp(o.path, e.path) == 0) {
						++dupes;
						break;
					}
				}
			}
			if (dupes) {
				logger::error(FMT_STRING("SCOPE TABLE: {} row(s) share a model path — the primary key is not "
				                         "unique and which row wins is arbitrary."),
					dupes);
			}
			if (!badKeys && !dupes) {
				logger::info(FMT_STRING("SCOPE TABLE: {} rows, keys verified unique and normalized"),
					std::size(kTable));
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

			// HOW it matched is as important as THAT it matched. "node" on a modded
			// load order is the case where the number can be confidently wrong --
			// a thermal scope whose mesh kept the root name "HuntingScope" gets the
			// hunting rifle's 1.267 and nothing about the result looks unusual.
			if (a_i.fromTable) {
				logger::info(FMT_STRING("SCOPE IDENT: matched by {} on '{}'{}"),
					a_i.matchedBy[0] ? a_i.matchedBy : "toml", a_i.matched,
					a_i.overrideKey[0] ? fmt::format(FMT_STRING(", overridden by [Scopes] entry '{}'"), a_i.overrideKey) : "");
			}
			if (a_i.fromTable && std::strcmp(a_i.matchedBy, "node") == 0 && a_i.haveMods) {
				logger::warn(FMT_STRING("SCOPE IDENT: matched by NODE NAME while the mod list WAS readable — "
				                        "none of the {} attached mod path(s) is in the built-in table, so this "
				                        "is most likely a modded optic wearing a vanilla mesh's root name and "
				                        "the aperture {:.3f} is probably wrong for it. Override it with a "
				                        "[Scopes] entry keyed on the model path (see the paths logged below)."),
					a_i.modCount, a_i.aperture);
			}

			// The attached mods, every probe. They are the raw material for a
			// [Scopes] entry and they cost one line; making a user re-run a probe
			// with a flag to see them is how the last diagnostic ended up needing
			// to be run in order to work.
			if (a_i.modCount) {
				std::string all;
				for (std::uint32_t m = 0; m < a_i.modCount; ++m) {
					if (m) {
						all += ", ";
					}
					all += fmt::format(FMT_STRING("{:08X}@{}:{}"), a_i.mods[m].formID, a_i.mods[m].attachPoint,
						a_i.mods[m].key[0] ? a_i.mods[m].key : "(no model)");
				}
				logger::info(FMT_STRING("SCOPE IDENT mods: {} attached, stack chosen by {} of {} seen{}: {}"),
					a_i.modCount, a_i.stackPick, a_i.stacksSeen,
					a_i.modOverflow ? fmt::format(FMT_STRING(" (+{} dropped, raise kMaxMods)"), a_i.modOverflow) : "",
					all);
			} else if (a_i.modsError[0]) {
				// Not a warning: a bare weapon legitimately has no mods. It is
				// logged so that "no mods" never has to be inferred from silence.
				logger::info(FMT_STRING("SCOPE IDENT mods: none read — {}"), a_i.modsError);
			}

			// Whether the exact census face resolved decides whether placement is a
			// measurement or a heuristic, and the difference is visible in VR. It
			// used to be inferable only from the auto-place line's "via" field; say
			// it here too, next to the scope it belongs to.
			if (a_i.fromTable && !a_i.haveFace) {
				logger::warn(FMT_STRING("SCOPE IDENT: '{}' has no usable census face (measured shape '{}' "
				                        "not in this weapon's 3D) — placement falls back to the bound "
				                        "heuristic, which is much rougher."),
					a_i.matched, a_i.faceShape[0] ? a_i.faceShape : "(none)");
			}

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
				if (a_i.scopeNameCount || a_i.modCount) {
					std::string all;
					// Model paths first, and labelled as preferred: a node-name
					// entry silently retunes every other mesh that shares the name,
					// which is the defect this version exists to close. Do not
					// offer the user the key we just stopped relying on ourselves.
					for (std::uint32_t m = 0; m < a_i.modCount; ++m) {
						if (a_i.mods[m].key[0]) {
							all += (all.empty() ? "" : ", ");
							all += '"';
							all += a_i.mods[m].key;
							all += '"';
						}
					}
					for (std::uint32_t i = 0; i < a_i.scopeNameCount; ++i) {
						all += (all.empty() ? "" : ", ");
						all += a_i.scopeNames[i];
					}
					logger::info(FMT_STRING("SCOPE IDENT: no table entry. Add one under [Scopes] in "
					                        "TrueScopesVR.toml keyed by one of (prefer a quoted model "
					                        "path — a node name may be shared with a vanilla scope): {}"),
						all);
				} else {
					// Nothing to key on AT ALL: no P-Scope subtree and no attached
					// mods. Either this weapon mounts its optic somewhere else, or
					// there is no optic on it. Distinguishable from "found it, do
					// not know it", and worth saying so.
					logger::info("SCOPE IDENT: no P-Scope attach point in this weapon's 3D and no "
					             "attached mods — nothing to key on, using the fallback aperture"sv);
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
		// Once per session, at the first probe rather than at static-init time so
		// the logger exists to receive the verdict.
		static std::once_flag verified;
		std::call_once(verified, VerifyTable);
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

	bool OcularFaceWorld(float (&a_world)[3])
	{
		const std::scoped_lock lock(g_lock);
		if (!g_info.probed || !g_info.haveFace) {
			return false;
		}
		for (std::uint32_t s = 0; s < g_info.shapeCount; ++s) {
			const auto& g = g_info.shapes[s];
			if (std::strcmp(g.name, g_info.faceShape) != 0) {
				continue;
			}

			// RE-READ THE SHAPE'S TRANSFORM LIVE (v0.2.99). The snapshot in
			// ShapeGeom was taken when the probe ran, which is a DIFFERENT FRAME
			// from the one the caller is placing in -- and the weapon moves. Mixing
			// the two put a persistent 0.3-0.5 unit error into the placement, which
			// the closed loop then chased and turned into a runaway.
			//
			// The name is re-checked against the live node before trusting it: a
			// weapon or mod swap can free this 3D, and a stale pointer that still
			// reads as memory would silently place the disc using another object's
			// transform. Name mismatch (or an unreadable name) means the snapshot
			// is what we have, and the caller is told the face is unavailable
			// rather than handed a plausible wrong answer.
			float       world[3] = { g.world[0], g.world[1], g.world[2] };
			float       rot[9];
			float       scale = g.scale;
			std::memcpy(rot, g.rot, sizeof(rot));
			if (g.node) {
				char live[kNameLen] = {};
				if (!NodeName(g.node, live) || std::strcmp(live, g.name) != 0) {
					return false;
				}
				const auto* w = reinterpret_cast<const float*>(g.node + kWorldTranslate);
				const auto* m = reinterpret_cast<const float*>(g.node + kWorldTransform);
				scale = *reinterpret_cast<const float*>(g.node + kWorldScale);
				for (std::size_t k = 0; k < 3; ++k) {
					world[k] = w[k];
				}
				for (std::size_t r = 0; r < 3; ++r) {
					for (std::size_t c = 0; c < 3; ++c) {
						rot[r * 3 + c] = m[c * kMatrixRowStride + r];  // transposed on read
					}
				}
			}

			// world = T + scale * (R * v), R row-major (already de-strided).
			if (!std::isfinite(scale) || scale < 1.0e-4f) {
				return false;
			}

			// GEOMETRIC INVARIANT: the face is a point ON this shape, so it cannot
			// lie outside the shape's own bounding sphere. The census measures its
			// distance from that sphere's centre; the radius comes from the node in
			// the live scene. Comparing the two is a real cross-check between the
			// offline table and the mesh actually loaded, not a restatement of the
			// table against itself.
			//
			// It fires when a modded mesh reuses a vanilla shape name with different
			// geometry — the collision risk in SESSION_2026-08-10 §5 — and falls
			// back to the heuristic, which is the right trade: a rougher placement
			// beats a confident wrong one.
			//
			// NOTE it must be measured from the BOUND CENTRE, not the shape origin.
			// The faces are in file coordinates and several scopes sit well off
			// their own mesh origin (MissileLauncherScope's face is 11.0 from it,
			// against a bound radius of 6.5), so an origin-based test would fire
			// constantly on perfectly good rows.
			if (g.boundRadius > 0.001f && g_info.faceFromCentre > g.boundRadius * 1.05f) {
				static bool warned = false;
				if (!warned) {
					warned = true;
					logger::warn(FMT_STRING("SCOPE IDENT: census face for '{}' sits {:.2f} from its bound "
					                        "centre but the live shape's bound radius is only {:.2f} — the "
					                        "row does not describe this mesh. Using the bound heuristic."),
						g_info.faceShape, g_info.faceFromCentre, g.boundRadius);
				}
				return false;
			}
			for (std::size_t r = 0; r < 3; ++r) {
				a_world[r] = world[r] + scale * (rot[r * 3 + 0] * g_info.face[0] +
				                                    rot[r * 3 + 1] * g_info.face[1] +
				                                    rot[r * 3 + 2] * g_info.face[2]);
				if (!std::isfinite(a_world[r])) {
					return false;
				}
			}
			return true;
		}
		return false;
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
