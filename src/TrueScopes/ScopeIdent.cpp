#include "TrueScopes/ScopeIdent.h"

#include "TrueScopes/Addresses.h"

#include "Settings/Settings.h"

namespace TrueScopes::ScopeIdent
{
	namespace
	{
		// engine entry points (RVAs, Fallout4VR.exe 1.2.72)
		constexpr std::uintptr_t kGetCurrentWeapon = 0xe50da0;   // Actor::GetCurrentWeapon(actor, out[2], equipIndex)
		constexpr std::uintptr_t kGetIronSightFOV = 0x332b80;    // TESObjectWEAP::GetIronSightFOV(weapon, instanceData) -> fovMult
		constexpr std::uintptr_t kGetZoomFOV = 0x332cb0;         // (weapon, baseFovDeg, instanceData) -> zoomed FOV in degrees
		constexpr std::uintptr_t kGetWeaponBone = 0xec5230;      // AIProcess::GetWeaponBone(process, actor, biped, equipIndex)
		constexpr std::uintptr_t kEquipIndexGlobal = 0x59444c8;  // u32; RIP-decoded from the MOV at 0x140efab3e (the decompiler's DAT_ label there is wrong)

		// object layout (read out of the VR binary's own code)
		constexpr std::uintptr_t kProcessInActor = 0x300;    // AIProcess* (FUN_140ef5be0: param_1[0x60])
		constexpr std::uintptr_t kGetBipedVFunc = 0x508;     // Actor::GetBiped(bool) -> BipedAnim**
		constexpr std::uintptr_t kNameInNiObjectNET = 0x10;  // BSFixedString (NiAVObject::GetObjectByName compares [+0x10])
		constexpr std::uintptr_t kChildrenInNiNode = 0x168;  // NiAVObject** (NiNode::GetObjectByName)
		constexpr std::uintptr_t kChildCountInNiNode = 0x172;  // u16
		constexpr std::uintptr_t kFormIDInTESForm = 0x14;
		// NiNode::GetObjectByName's own address. A node's vtable slot 0x188 holding
		// this exact pointer is how a child is known to be an NiNode and therefore
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

		// the attached-mod chain
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
		// Cross-checked: DevBench's OMOD string scan reports the texture-ID block
		// at +0x58, and TESModel::textures is at TESModel+0x10 -- 0x48 + 0x10 =
		// 0x58, so the base offset is not a guess.
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

		// Per-scope ocular radius and face position, measured from the shipped
		// first-person meshes by tools/scope-census.py. The "via" comment on each
		// row says how it was measured: "glass" = a real clear-aperture shape,
		// "screen" = flat-quad optics measured off the display quad (the boxy
		// housing reads 1.5-5x too wide), tube rows are measured at the ocular
		// and read ~15% high -- starting points, which is why [Scopes] can
		// override them.
		//
		// Colliding keys are resolved by shape, not by order. Three meshes name
		// their root "LaserScope"; all three rows are present, and Resolve()
		// picks the one whose measured shape is actually in the weapon's 3D. A
		// key match with no shape match still supplies the aperture but no face,
		// and says so in the log.
		//
		// The face is in the measured shape's own space -- file coordinates,
		// whose origin is the runtime node's origin -- so it must be pushed
		// through that shape's live world transform, hence the shape name in the
		// row. A mesh whose shapes moved between versions fails to resolve
		// instead of placing the disc confidently in the wrong spot.
		//
		// Generated by `python tools/scope-census.py --cpp`.
		constexpr TableEntry kTable[] = {
			// Census rules: rear-rim for tube optics, paired-quad for screens,
			// rectangular-screen aspect; rows verified in VR are pinned at their
			// verified values. DLC rows measured from the DLCCoast/DLCNukaWorld
			// BA2s.
		// model path (primary key), node name (fallback key), aperture,
		// screen aspect (h/w; 1 = circular or square screen), measured shape,
		// ocular face (file coords), |face - bound centre|
		{ "weapons/machinegun/mgscope.nif", "MachineGunScope", 0.649f, 1.000f, "MachineGunScope:6", { -0.013f, -5.457f, 3.379f }, 4.945f },  // MGScope_1.nif via rim-ring
		{ "weapons/laserweapons/institute/scope.nif", "Scope", 0.825f, 1.000f, "Scope:0", { 0.769f, -13.750f, 5.158f }, 4.902f },  // Scope_1.nif via rim
		{ "dlc04/weapons/raiderguns/scopereflexcircle.nif", "ScopeReflexCircle", 0.875f, 1.000f, "TubeGlass003:0", { 0.000f, -0.138f, -0.069f }, 0.138f },  // ScopeReflexCircle_1.nif via glass
		{ "dlc04/weapons/raiderguns/scopereflexdot.nif", "ScopeReflexDot", 0.875f, 1.000f, "TubeGlass004:0", { 0.000f, -0.138f, -0.069f }, 0.138f },  // ScopeReflexDot_1.nif via glass
		{ "weapons/laserweapons/laserscopenobracket.nif", "LaserScope002", 0.924f, 1.000f, "LaserScope002:0", { 0.001f, -2.957f, 2.528f }, 4.503f },  // LaserScopeNoBracket.nif via rim
		{ "weapons/railwayrifle/railwayriflescope.nif", "RailwayRifleScope", 0.960f, 1.000f, "RailwayRifleScope:0", { -0.009f, -0.773f, 0.933f }, 4.857f },  // RailwayRifleScope_1.nif via rim
		{ "dlc03/weapons/levergun/4570scope.nif", "4570Scope", 0.999f, 1.000f, "4570Scope:1", { -0.027f, -5.480f, 1.097f }, 7.684f },  // 4570Scope_1.nif via rim
		{ "weapons/gaussrifle/gaussriflescope.nif", "GaussRifleScope", 1.028f, 1.000f, "GaussRifleScope:0", { 0.022f, -3.260f, 1.500f }, 2.424f },  // GaussRifleScope_1.nif via pinned
		{ "dlc04/weapons/raiderguns/scopelong.nif", "ScopeLong", 1.083f, 1.000f, "ScopeLong:0", { 0.005f, -19.609f, 1.828f }, 9.846f },  // ScopeLong_1.nif via pinned
		{ "dlc04/weapons/raiderguns/scopeshort.nif", "ScopeShort", 1.083f, 1.000f, "ScopeShort:0", { 0.005f, -19.609f, 1.828f }, 9.436f },  // ScopeShort_1.nif via pinned
		{ "weapons/missilelauncher/missilelauncherscope.nif", "MissileLauncherScope", 1.105f, 1.000f, "MissileLauncherScope:0", { -10.101f, -2.869f, 3.277f }, 5.961f },  // MissileLauncherScope_1.nif via pinned
		{ "weapons/laserweapons/laserscope.nif", "LaserScope", 1.175f, 1.000f, "LaserScope:0", { -0.007f, -8.539f, 3.389f }, 6.432f },  // LaserScope_1.nif via pinned
		{ "weapons/lasermusket/lasermusketlaserscope.nif", "LaserScope", 1.175f, 1.000f, "LaserScope:1", { -0.010f, -8.539f, 3.733f }, 6.430f },  // LaserMusketLaserScope.nif via pinned
		{ "weapons/reconscope/pistol_scope_44mag.nif", "44MagReconScope", 1.206f, 1.000f, "Screen023:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_scope_44Mag.nif via screen
		{ "weapons/reconscope/pistol_scope_gauss.nif", "GaussRifleReconScope", 1.206f, 1.000f, "Screen024:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_Scope_Gauss.nif via screen
		{ "weapons/reconscope/pistol_scope_laser.nif", "LaserScope", 1.206f, 1.000f, "Screen021:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_scope_laser.nif via screen
		{ "weapons/reconscope/pistol_scope_plasma.nif", "PlasmaScope002", 1.206f, 1.000f, "Screen019:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_scope_Plasma.nif via screen
		{ "weapons/reconscope/pistol_scope_railway.nif", "Pistol_scope_Railway", 1.206f, 1.000f, "Screen018:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Pistol_scope_Railway.nif via screen
		{ "weapons/reconscope/recon_pistol_scope.nif", "Pistol_scope", 1.206f, 1.000f, "Screen020:1", { 0.000f, -0.004f, -0.001f }, 0.000f },  // Recon_Pistol_scope.nif via screen
		{ "dlc03/weapons/gammarifle/gammariflescope.nif", "GammaRifleScope", 1.267f, 1.000f, "HuntingScope:2", { 0.000f, -5.289f, 1.814f }, 7.767f },  // GammaRifleScope_1.nif via glass
		{ "weapons/huntingrifle/huntingriflescope.nif", "HuntingScope", 1.267f, 1.000f, "HuntingScope:2", { 0.000f, -6.422f, 1.814f }, 11.043f },  // HuntingRifleScope_1.nif via glass
		{ "weapons/machinegun/mgscopethermal.nif", "MGThermalScope", 1.273f, 0.538f, "Screen023:0", { 0.000f, 0.004f, 0.000f }, 0.000f },  // MGScopeThermal_1.nif via screen
		{ "weapons/44/44scope.nif", "44MagScope", 1.290f, 1.000f, "44MagScope:0", { -0.002f, -8.539f, 2.118f }, 8.657f },  // 44Scope_1.nif via rim
		{ "dlc04/weapons/44western/44magscopewestern.nif", "44MagScopeWestern", 1.290f, 1.000f, "44MagScopeWestern:0", { -0.002f, -8.539f, 2.118f }, 8.657f },  // 44MagScopeWestern.nif via rim
		{ "weapons/alienblaster/scope01.nif", "Scope01", 1.439f, 1.000f, "Scope01:0", { 0.000f, -5.059f, 2.032f }, 6.666f },  // Scope01_1.nif via rim
		{ "weapons/handmade/scopes/handmadescopezoom.nif", "HandMadeScope", 1.470f, 1.000f, "HandMadeScope:0", { 0.007f, -6.715f, 2.618f }, 8.078f },  // HandMadeScopeZoom_1.nif via rim
		{ "weapons/reconscope/reconscoperifle.nif", "ReconScopeRifle", 1.484f, 1.000f, "Screen016:1", { 0.006f, -6.816f, 5.340f }, 0.002f },  // ReconScopeRifle.nif via screen
		{ "weapons/reconscope/rifle_scope_assault.nif", "AssaultRifleScope", 1.484f, 1.000f, "Screen017:1", { 0.006f, -6.816f, 5.340f }, 0.002f },  // Rifle_Scope_Assault.nif via screen
		{ "weapons/reconscope/rifle_scope_institute.nif", "InstituteReconScope", 1.484f, 1.000f, "Screen018:1", { 0.006f, -6.816f, 5.340f }, 0.002f },  // Rifle_Scope_Institute.nif via screen
		{ "dlc03/weapons/harpoongun/harpoongunscope.nif", "Scope", 1.494f, 1.000f, "Scope:0", { -1.180f, -15.258f, 5.004f }, 15.615f },  // HarpoonGunScope_1.nif via rim
		{ "weapons/machinegun/scopes/machinegunscopef.nif", "Box121", 1.576f, 1.000f, "Box121:6", { -0.020f, -5.527f, 3.350f }, 5.019f },  // MachineGunScopeF.nif via rim
		{ "weapons/handmade/scopes/handmadescope.nif", "PipeReflex", 1.597f, 1.000f, "PipeReflex:0", { 0.175f, -3.750f, 2.415f }, 2.883f },  // HandmadeScope_1.nif via pinned
		{ "weapons/fatman/fatmanscope.nif", "FatmanScope", 2.159f, 1.000f, "FatmanScope:0", { -2.223f, -6.680f, -7.689f }, 4.826f },  // FatmanScope_1.nif via pinned
		{ "weapons/plasma/scope.nif", "PlasmaScope", 3.030f, 1.000f, "PlasmaScope:1", { 0.000f, -2.012f, -0.079f }, 6.729f },  // Scope_1.nif via rim-ring
		};

		// Long-eye-relief optics: pistol scopes are built to be viewed at arm's
		// length, and the eyebox's relief centre must sit there or the ring
		// collapses the picture at the weapon's own natural hold. Keyed like the
		// census table (path primary, node fallback); [Scopes] eyeRelief
		// overrides. ~30 units = 43 cm, a scout-scope relief.
		struct ReliefEntry
		{
			const char* path;
			const char* node;
			float       relief;
		};
		constexpr ReliefEntry kLongRelief[] = {
			{ "weapons/44/44scope.nif", "44MagScope", 30.0f },
			{ "dlc04/weapons/44western/44magscopewestern.nif", "44MagScopeWestern", 30.0f },
		};

		// Walk bounds. Generous relative to a weapon (the hunting rifle is 50 nodes,
		// 6 deep) but still bounded, so a corrupt child count or a cyclic graph costs
		// a fixed number of reads instead of hanging the probing thread.
		constexpr int          kMaxDepth = 16;
		constexpr std::uint32_t kMaxNodes = 2048;

		std::atomic_bool g_request{ true };  // probe once at the first eligible frame
		std::mutex       g_lock;
		Info             g_info;

		// Templated on the buffer size: node names are 64 bytes and model paths
		// are 128, and picking the wrong one is a silent truncation that would
		// turn a path key into a near-miss.
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

		// A BSFixedString field holds a BSStringPool::Entry*, not a char*:
		//     entry+0x00  next/pool pointer
		//     entry+0x10  u32 length
		//     entry+0x18  the characters
		// Validate rather than trust: the declared length must agree with where
		// the terminator actually sits, and every character must be printable. A
		// field that fails is reported as no name -- a visible miss beats a
		// plausible-looking lie.
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

		// The scope hangs off the receiver's "P-Scope" connect point. Table keys
		// are matched against P-Scope's subtree, not the whole weapon -- a key
		// like "Scope" would otherwise happily match some unrelated receiver or
		// sight node and fit the lens to it. Note the mod NIF's root node is not
		// instantiated at runtime: its shapes attach directly as "<root>:N", so
		// names are stripped at the colon to recover the offline census keys.
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

		// geometry capture

		void ReadGeom(std::uintptr_t a_node, const char* a_name, ShapeGeom& a_out)
		{
			a_out.node = a_node;
			CopyName(a_out.name, a_name);
			const auto* w = reinterpret_cast<const float*>(a_node + kWorldTranslate);
			a_out.world[0] = w[0];
			a_out.world[1] = w[1];
			a_out.world[2] = w[2];
			a_out.scale = *reinterpret_cast<const float*>(a_node + kWorldScale);
			// Transposed on read. The engine stores this matrix column-major: the
			// true mapping is world = T + s * (M_stored^T * v). Reading the
			// transpose here means rot[] is the real rotation, so every consumer
			// -- and the pScopeRot published by DevBench -- can use the textbook
			// form. A rotation applied the wrong way round preserves length, so
			// magnitude checks do not catch the mistake; see the ShapeGeom note
			// in the header.
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
			// Both caps count what they refuse: a cap that silently prunes turns
			// "the scope is not in this weapon's 3D" and "the walk stopped
			// looking" into the same output.
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

			// Record where it is, not just what it is called.
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

		// attached object mods

		// Every attached OMOD on the equipped weapon, with its model path. The
		// path is the only key that is unique per mesh, which is what makes a
		// modded optic distinguishable from the vanilla one whose root node name
		// it copied. Declines with a reason rather than an empty list: "this
		// weapon has no mods" and "the layout moved and nothing was read" are
		// different facts and have to look different.
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

			// A base weapon can sit in the inventory several times with different
			// mods, so the stack is chosen by identity: the one whose instance
			// data is the object Actor::GetCurrentWeapon just returned. Stack 0
			// could be a spare rifle in the player's pack.
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
				// same TBO_InstanceData object the inventory stack holds, which
				// has not been confirmed live. With exactly one stack the question
				// is moot, so take it and record "sole". If every probe reports
				// "sole", the identity rule never fires and the comparison needs
				// revisiting.
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
			// at buffer+size (one 4-byte entry for N=1): size:24 in the low bits,
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
			__try {
				if (inst[0]) {
					a_out.weaponFormID = *reinterpret_cast<std::uint32_t*>(inst[0] + kFormIDInTESForm);
					a_out.fovMult = Fn<GetIronSightFOV_t>(kGetIronSightFOV)(inst[0], inst[1]);
					a_out.zoomFovAt90 = Fn<GetZoomFOV_t>(kGetZoomFOV)(inst[0], 90.0f, inst[1]);
					// zoomData: instance data first (a scope OMOD's zoom overrides
					// the base), then the weapon form; the same selection
					// TS_ScopeMenu_ProcessMessage makes (inst+0x90 / weap+0x228).
					// BGSZoomData::Data at +0x20: {fovMult, overlay +0x24, imod +0x28}.
					const auto zoomData = inst[1] && *reinterpret_cast<std::uintptr_t*>(inst[1] + 0x90)
					                          ? *reinterpret_cast<std::uintptr_t*>(inst[1] + 0x90)
					                          : *reinterpret_cast<std::uintptr_t*>(inst[0] + 0x228);
					if (zoomData) {
						// overlay index: the exact read TS_ScopeMenu_ProcessMessage makes.
						a_out.zoomOverlay = *reinterpret_cast<std::uint32_t*>(zoomData + 0x24);
						// the resolved imod pointer is +0x38 (the +0x28 slot is the raw
						// formID from the ESM); +0x38 is what the engine's own Trigger
						// call site uses, null for every non-NV/non-recon zoom.
						if (const auto imod = *reinterpret_cast<std::uintptr_t*>(zoomData + 0x38)) {
							a_out.zoomImodID = *reinterpret_cast<std::uint32_t*>(imod + kFormIDInTESForm);
						}
					}
				}
				// Before the release: the instance-data pointer is what identifies the
				// equipped inventory stack, and the held reference keeps it alive for
				// the comparison.
				ReadAttachedMods(a_player, inst[0], inst[1], a_out);
			} __finally {
				// termination handler, like the decal stage's: a fault in the walk
				// above unwinds to ProbeGuarded, and the held reference must not
				// leak with it.
				ReleaseInstanceData(inst[1]);
			}

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
		// decides whether the census face is usable -- the face is expressed in
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
			a_out.screenAspect = a_e.aspect;
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
			// the warning in LogInfo names, so "no face" says which shape is missing.
		}

		// The aperture table lookup, in strict order of evidence quality.
		void Resolve(Info& a_out)
		{
			a_out.aperture = static_cast<float>(*Settings::widgetApertureRadius);
			a_out.screenAspect = 1.0f;
			a_out.fromTable = false;
			a_out.matched[0] = '\0';
			a_out.matchedBy[0] = '\0';
			a_out.overrideKey[0] = '\0';
			if (!*Settings::perScopeAperture) {
				return;
			}

			// 1. by model path
			// The only key that is unique per mesh. A modded optic that copied a
			// vanilla root node name has its own path, so this is the pass that
			// stops it being handed the vanilla numbers. Every attached mod is
			// tried, not just a guessed scope slot: receiver, barrel and muzzle
			// mods simply are not in a table of scopes, so the census itself
			// does the selecting.
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

			// 2. by node name
			// Still needed: an optic welded into a weapon's base mesh is not an
			// OMOD at all and has no path to key on, and the mod chain can
			// decline for the ordinary reasons in Info::modsError. Second because
			// it is the weaker evidence, and only names from under P-Scope are
			// candidates.
			//
			// Among rows sharing a node key, prefer the one whose measured shape
			// is actually in the scene: three shipped meshes call their root node
			// "LaserScope", and the shape name, checked against the live 3D, is
			// what distinguishes them. A key match with no shape match still
			// supplies the aperture (better than the global fallback) but no
			// face, and the warning in LogInfo says placement has dropped to the
			// heuristic.
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

			// 3. TOML overrides, applied on top
			// An entry may carry a radius, a position, or both -- offsets alone
			// mean "the built-in radius is fine, where you put it is not", and an
			// aperture alone means the reverse. Neither disturbs the census face
			// resolved above. The overrides must run after the table passes: the
			// table is what resolves the face, and an override that returned
			// early would silently downgrade placement to the bounding-sphere
			// heuristic. Model paths are tried before node names for the same
			// reason the table tries them first: an entry keyed on a node name
			// retunes every mesh that copied that root name.
			// Built-in long-relief pass, before the TOML so an override wins.
			for (const auto& r : kLongRelief) {
				if ((a_out.matched[0] && (std::strcmp(r.path, a_out.matched) == 0 ||
										     std::strcmp(r.node, a_out.matched) == 0))) {
					a_out.eyeRelief = r.relief;
					break;
				}
			}

			const auto applyOverride = [&a_out](const char* a_key) {
				const auto e = Settings::ScopeEntryFor(a_key);
				const bool haveOffset = !std::isnan(e.offsetX) || !std::isnan(e.offsetY) || !std::isnan(e.offsetZ);
				if (e.eyeRelief > 0.0) {
					a_out.eyeRelief = static_cast<float>(e.eyeRelief);
				}
				if (e.aperture <= 0.0 && !haveOffset) {
					return e.eyeRelief > 0.0;
				}
				CopyName(a_out.overrideKey, a_key);
				a_out.offsetX = static_cast<float>(e.offsetX);
				a_out.offsetY = static_cast<float>(e.offsetY);
				a_out.offsetZ = static_cast<float>(e.offsetZ);
				if (e.aperture > 0.0) {
					a_out.aperture = static_cast<float>(e.aperture);
					a_out.fromTable = true;
				}
				if (e.aspect > 0.0) {
					a_out.screenAspect = static_cast<float>(e.aspect);
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

		// Protects the path key, run once at the first probe. The table's keys
		// are produced by mesh_key() in tools/scope-census.py and consumed by
		// Settings::NormalizeScopeKeyInto here, and nothing in the build connects
		// the two -- if they ever disagree the lookup does not fail, it quietly
		// falls through to the node-name pass and looks exactly like a table that
		// works. A normalized key must be a fixed point of the normalizer, so
		// Normalize(row.path) == row.path is a direct test of agreement on the
		// real data. Uniqueness is checked in the same pass because being unique
		// is the path key's entire justification.
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

			// How it matched is as important as that it matched. "node" on a modded
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
			// [Scopes] entry and they cost one line.
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
			// measurement or a heuristic, and the difference is visible in VR.
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
			// scope may simply be in the part that was dropped. Say so rather than
			// letting it read as "this scope is unknown".
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

			// When nothing matched, the names are the finding: they are what a user
			// needs in order to add a [Scopes] entry. Print them rather than making
			// someone attach a debugger to see them.
			if (!a_i.fromTable) {
				if (a_i.scopeNameCount || a_i.modCount) {
					std::string all;
					// Model paths first, and labelled as preferred: a node-name
					// entry silently retunes every other mesh that shares the name.
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
					// Nothing to key on at all: no P-Scope subtree and no attached
					// mods -- either this weapon mounts its optic somewhere else,
					// or there is no optic on it. Distinct from "found it, do not
					// know it".
					logger::info("SCOPE IDENT: no P-Scope attach point in this weapon's 3D and no "
					             "attached mods — nothing to key on, using the fallback aperture"sv);
				}
			}
		}
	}


	PathLookup LookupModelPath(const char* a_rawPath) noexcept
	{
		PathLookup r{};
		r.aspect = 1.0f;
		if (!a_rawPath) {
			return r;
		}
		Settings::NormalizeScopeKeyInto(a_rawPath, r.key, sizeof(r.key));
		for (const auto& e : kTable) {
			if (std::strcmp(e.path, r.key) == 0) {
				r.hit = true;
				r.aperture = e.aperture;
				r.aspect = e.aspect;
				std::snprintf(r.node, sizeof(r.node), "%s", e.node);
				std::snprintf(r.shape, sizeof(r.shape), "%s", e.shape);
				break;
			}
		}
		// [Scopes] override on top, same as Resolve() step 3 (aperture/aspect —
		// offsets are placement data with no meaning in a pure lookup).
		const auto o = Settings::ScopeEntryFor(r.key);
		if (o.aperture > 0.0) {
			r.hit = true;
			r.fromToml = true;
			r.aperture = static_cast<float>(o.aperture);
		}
		if (o.aspect > 0.0) {
			r.fromToml = true;
			r.aspect = static_cast<float>(o.aspect);
		}
		return r;
	}


	AttachOutcome AttachModToEquippedWeapon(std::uint32_t a_modFormID, bool a_attach) noexcept
	{
		AttachOutcome r{};
		const auto base = REL::Module::get().base();
		constexpr std::uintptr_t kPlayerGlobal = Addr::kPlayerGlobal;
		constexpr std::uintptr_t kModifyInventoryItemMod = 0x1488a00;  // GameScript::ModifyInventoryItemMod
		constexpr std::uintptr_t kGetInventoryObjectCount = 0x3e3750;  // TESObjectREFR::GetInventoryObjectCount(ref, form)
		using ModifyInvMod_t = bool (*)(void*, std::uint32_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, bool);
		using InvCount_t = std::uint32_t (*)(std::uintptr_t, std::uintptr_t);

		const auto fail = [&r](const char* a_msg) {
			std::snprintf(r.error, sizeof(r.error), "%s", a_msg);
			return r;
		};
		const auto player = *reinterpret_cast<std::uintptr_t*>(base + kPlayerGlobal);
		if (!player) {
			return fail("no player");
		}
		// The engine validates the OMOD type itself: GetModFromID returns null for
		// anything that is not a BGSMod::Attachment::Mod.
		const auto mod = Fn<GetModFromID_t>(kGetModFromID)(a_modFormID);
		if (!mod) {
			return fail("formID is not an OMOD (GetModFromID returned null)");
		}
		std::uintptr_t inst[2] = { 0, 0 };
		Fn<GetCurrentWeapon_t>(kGetCurrentWeapon)(player, inst, *reinterpret_cast<std::uint32_t*>(base + kEquipIndexGlobal));
		ReleaseInstanceData(inst[1]);
		if (!inst[0]) {
			return fail("no equipped weapon");
		}
		r.weaponFormID = *reinterpret_cast<std::uint32_t*>(inst[0] + kFormIDInTESForm);
		// The engine's own preconditions, checked here so its error paths (which
		// dereference the null VM passed below) are unreachable.
		const auto count = Fn<InvCount_t>(kGetInventoryObjectCount)(player, inst[0]);
		if (count == 0) {
			return fail("weapon not in inventory");
		}
		if (count > 1) {
			std::snprintf(r.error, sizeof(r.error),
				"GetInventoryObjectCount=%u - engine can only mod singular items", count);
			return r;
		}
		r.ok = Fn<ModifyInvMod_t>(kModifyInventoryItemMod)(nullptr, 0, player, inst[0], mod, a_attach);
		if (!r.ok) {
			std::snprintf(r.error, sizeof(r.error), "engine ModifyInventoryItemMod returned false");
		}
		return r;
	}

	static std::atomic<std::uint32_t> g_probeCount{ 0 };

	// The internal IsNode test, exported for the pill cull (Hooks.cpp). Exactly
	// the 20-class NiNode scene family (NiNode, BSFadeNode, BSOrderedNode,
	// NiSwitchNode, BSMultiBoundNode, NiBillboardNode, ShadowSceneNode, ...)
	// carries NiNode::GetObjectByName at +0x188 and no subclass overrides it, so
	// this is an exact "safe to read the children array" test, not a heuristic.
	// The reads are unguarded -- call it under SEH like every other scene read.
	bool IsNiNode(std::uintptr_t a_obj) noexcept
	{
		return a_obj != 0 && IsNode(a_obj);
	}

	void Request()
	{
		g_request.store(true);
	}

	bool ProbePending()
	{
		return g_request.load();
	}

	std::uint32_t ProbeCount()
	{
		return g_probeCount.load(std::memory_order_relaxed);
	}

	bool CensusFaceExpected()
	{
		const std::scoped_lock lock(g_lock);
		// A face is "expected" when the table row that matched this scope names a
		// face shape. faceShape is copied whether or not the shape resolved (the
		// no-usable-face warning depends on that), so this is exactly "the census
		// promised placement data for this optic". A faulted walk latches
		// immediately: probing is disabled, retries could never improve it.
		return g_info.probed && !g_info.faulted && g_info.fromTable && g_info.faceShape[0] != 0;
	}

	bool CensusFaceResolved()
	{
		const std::scoped_lock lock(g_lock);
		return g_info.probed && g_info.haveFace;
	}

	void RunIfRequested(std::uintptr_t a_player)
	{
		if (!a_player || !g_request.exchange(false)) {
			return;
		}
		// One probe body at a time. Two call sites exist (the verdict thunk on
		// the game thread and the fill-hook fallback on the render thread), and
		// two requests can be in flight; the loser re-arms the request and lets
		// the next eligible frame serve it.
		static std::atomic_bool s_busy{ false };
		if (s_busy.exchange(true)) {
			g_request.store(true);
			return;
		}
		struct BusyClear
		{
			std::atomic_bool& b;
			~BusyClear() { b.store(false); }
		} clear{ s_busy };
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
			// reset first: a previous weapon's probed values must not survive
			// the latch, or its aperture/offsets/fovMult keep applying. probed
			// goes false, so every accessor falls back to the global settings.
			g_info = Info{};
			g_info.faulted = true;
			logger::error("SCOPE IDENT: the weapon 3D walk faulted — per-scope fitting disabled for this session"sv);
			return;
		}

		info.probed = true;
		Resolve(info);
		{
			const std::scoped_lock lock(g_lock);
			g_info = info;
			g_probeCount.fetch_add(1, std::memory_order_relaxed);
		}
		LogInfo(info);
	}

	std::uint32_t CurrentWeaponFormID()
	{
		const std::scoped_lock lock(g_lock);
		return g_info.probed ? g_info.weaponFormID : 0;
	}

	void InvalidateNodes()
	{
		const std::scoped_lock lock(g_lock);
		g_info.weaponNode = 0;
		g_info.pScopeNode = 0;
		g_info.haveFace = false;  // OcularFaceWorld declines; placement falls back
		g_info.haveGeom = false;  // ScopeBound declines
		g_info.boundsSeen = 0;
		for (auto& s : g_info.shapes) {
			s.node = 0;
		}
	}

	void InvalidateForLifecycle()
	{
		{
			const std::scoped_lock lock(g_lock);
			// A guarded weapon-3D walk fault is intentionally session-latched;
			// lifecycle churn must not repeatedly retry the same unsafe walk.
			const bool faulted = g_info.faulted;
			g_info = Info{};
			g_info.faulted = faulted;
		}
		// Publish the empty answer before requesting its replacement. Accessors
		// fall back to global settings until the game-thread verdict site serves
		// this request; the widget lifecycle gate keeps that interim state hidden.
		g_request.store(true, std::memory_order_release);
	}

	float ApertureRadius()
	{
		const std::scoped_lock lock(g_lock);
		// Before the first probe, and whenever per-scope fitting is off or the
		// walk faulted: just the one global setting.
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

	float EyeReliefUnits()
	{
		const std::scoped_lock lock(g_lock);
		return g_info.probed ? g_info.eyeRelief : 0.0f;
	}

	// OcularFaceWorld's live node re-read runs per placement on the game thread,
	// outside the probe's SEH. A freed node can fault between the name re-check
	// and the transform reads. POD frame only; a fault reads as "face
	// unavailable" and the caller declines the placement.
	static bool ReadFaceLive(std::uintptr_t a_node, const char* a_want,
		float (&a_world)[3], float (&a_rot)[9], float& a_scale) noexcept
	{
		__try {
			char live[kNameLen] = {};
			if (!NodeName(a_node, live) || std::strcmp(live, a_want) != 0) {
				return false;
			}
			const auto* w = reinterpret_cast<const float*>(a_node + kWorldTranslate);
			const auto* m = reinterpret_cast<const float*>(a_node + kWorldTransform);
			a_scale = *reinterpret_cast<const float*>(a_node + kWorldScale);
			for (std::size_t k = 0; k < 3; ++k) {
				a_world[k] = w[k];
			}
			for (std::size_t r = 0; r < 3; ++r) {
				for (std::size_t c = 0; c < 3; ++c) {
					a_rot[r * 3 + c] = m[c * kMatrixRowStride + r];  // transposed on read
				}
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
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

			// Re-read the shape's transform live: the ShapeGeom snapshot is from
			// the probe's frame, not the one the caller is placing in, and the
			// weapon moves between the two. The name is re-checked against the
			// live node before trusting it: a weapon or mod swap can free this
			// 3D, and a stale pointer that still reads as memory would silently
			// place the disc using another object's transform -- on a mismatch
			// the caller is told the face is unavailable rather than handed a
			// plausible wrong answer.
			float       world[3] = { g.world[0], g.world[1], g.world[2] };
			float       rot[9];
			float       scale = g.scale;
			std::memcpy(rot, g.rot, sizeof(rot));
			if (g.node) {
				if (!ReadFaceLive(g.node, g.name, world, rot, scale)) {
					return false;  // freed/renamed node, or a faulting read
				}
			}

			// world = T + scale * (R * v), R row-major (already de-strided).
			if (!std::isfinite(scale) || scale < 1.0e-4f) {
				return false;
			}

			// Geometric invariant: the face is a point on this shape, so it cannot
			// lie outside the shape's own bounding sphere. The census measures its
			// distance from that sphere's centre; the radius comes from the node
			// in the live scene, so this cross-checks the offline table against
			// the mesh actually loaded. It fires when a modded mesh reuses a
			// vanilla shape name with different geometry, and falls back to the
			// heuristic -- a rougher placement beats a confident wrong one.
			// Measured from the bound centre, not the shape origin: several
			// scopes sit well off their own mesh origin (MissileLauncherScope's
			// face is 11.0 from it, against a bound radius of 6.5), so an
			// origin-based test would fire constantly on perfectly good rows.
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

	bool OcularShapeRotation(float (&a_rot)[9])
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
			if (!g.node) {
				return false;
			}
			float world[3];
			float scale = 0.0f;
			if (!ReadFaceLive(g.node, g.name, world, a_rot, scale)) {
				return false;
			}
			for (const float v : a_rot) {
				if (!std::isfinite(v)) {
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
