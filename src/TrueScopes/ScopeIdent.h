#pragma once

// WHICH SCOPE IS EQUIPPED, AND HOW MUCH DOES IT MAGNIFY (v0.2.85)
//
// The widget disc has to be fitted to the real scope's ocular lens, and the
// render FOV has to match the scope's magnification. Both are per-scope, and
// the spread is large: measured across the 26 shipped weapon scopes the ocular
// radius runs 0.76 - 4.56, i.e. a 6x range of ScopeParent scale. A single
// global constant (what v0.2.68-84 shipped) fits exactly one weapon.
//
// So the plugin has to answer "which scope is this?" at runtime. It does that
// by walking the equipped weapon's 3D and reading node names: a weapon mod's
// model is attached under the weapon bone, and the attached subtree keeps the
// NIF's own root node name -- "HuntingScope", "44MagScope", "PlasmaScope" and
// so on. Those names are distinct for 24 of the 26 shipped scopes (the only
// collision is "LaserScope", shared by the laser rifle scope and the laser
// pistol recon scope), which makes the node name a usable table key.
//
// Everything here is derived from vanilla code paths rather than invented:
//   Actor::GetCurrentWeapon      0x140e50da0  -> {TESObjectWEAP*, InstanceData*}
//   TESObjectWEAP::GetIronSightFOV 0x140332b80 -> zoomData->fovMult (default 1.0)
//   "GetZoomFOV"                 0x140332cb0  -> 2*atan(tan(fov/2)/fovMult)
//   AIProcess::GetWeaponBone     0x140ec5230  -> the equipped weapon's node
// and the equip-index global is 0x1459444c8, RIP-decoded from the call site at
// 0x140efab3e ("MOV R8D, dword ptr [0x1459444c8]") rather than taken from a
// Ghidra DAT_ label -- the decompiler prints 0x1458e2998 there, which is wrong,
// the same high-.data mislabelling documented in the investigation repo.
//
// -------------------------------------------------------------------------
// v0.2.101: THE NODE NAME IS NOT A SAFE KEY ONCE MODS ARE INSTALLED.
//
// Node names are distinct across the 26 SHIPPED optics (one collision), which
// is what made them look usable. They are not distinct across a modded load
// order, because modders copy a vanilla mesh and keep its root node name.
// Censused against the user's own mods (SESSION_2026-08-10_PER_SCOPE_FIT.md §5):
//
//     ScopeTherm1_1.nif   (a thermal scope) -> root "HuntingScope"  -> 1.267
//     SCARScopeLarge.nif                    -> root "44MagScope"    -> 1.290
//     Scope_3_1.nif                         -> root "PlasmaScope"   -> 4.564
//     HarpoonGunScope_1.nif                 -> root "Scope"         -> 3.058
//
// So a name-keyed table hands a modded optic the vanilla numbers with complete
// confidence -- and a [Scopes] override could not correct it without also
// breaking the vanilla scope it collides with.
//
// The fix is to key on the MODEL PATH, which is unique per mesh. Two shipped
// scopes are both called Scope_1.nif (Plasma and the Institute laser), so it
// has to be the whole relative path, not the file name.
//
// Reaching it at runtime is this chain, all of it read out of the VR binary
// (QueueFiles 0x140eda410 does exactly these four steps in order):
//
//   TESObjectREFR::GetInventoryItem  0x1403e6270  (player, weapon) -> BGSInventoryItem*
//   BGSInventoryItem::GetExtraDataAt 0x1401aeeb0  (item, stackIdx) -> ExtraDataList*
//   ExtraDataList::GetExtraData      0x1400442f0  (list, 0x35)     -> BGSObjectInstanceExtra*
//   BGSObjectInstanceExtra::GetModFromID 0x14003d650 (u32)         -> BGSMod::Attachment::Mod*
//
// then the mod's TESModel is at +0x48 and its path BSFixedString at +0x50.
// That offset has an independent cross-check: DevBench's OMOD string scan has
// always reported the texture-ID block at +0x58, and TESModel::textures sits at
// TESModel+0x10 -- 0x48 + 0x10 = 0x58, from two unrelated observations.
//
// WHICH STACK: a base weapon can be in the inventory several times with
// different mods, so the stack is chosen by IDENTITY -- the one whose
// ExtraDataList::GetInstanceData (0x14008b040) pointer-equals the InstanceData
// that Actor::GetCurrentWeapon just handed back. Not by "stack 0", which would
// silently describe a spare rifle in the player's pack.
//
// WHY THE PATH NEEDS NORMALIZING: the OMOD carries the WORLD model path and the
// engine substitutes "_1.nif" for ".nif" to get the first-person mesh -- proven
// at FUN_14130c910, which literally does BSstristr(buf, ".nif") then
// strcat_s(buf, "_1.nif"). The census measures the _1 mesh, the runtime hands
// us the other name for the same optic, so both are folded to one key. See
// NormalizeModelPath, whose rules must stay identical to mesh_key() in
// tools/scope-census.py.

namespace TrueScopes::ScopeIdent
{
	// v0.2.88: was 40, which was not a safe margin -- it was less than one
	// weapon. The hunting rifle's 3D walked to 50 nodes, so the first live probe
	// stored the receiver, bolt, magazine and trigger and threw the scope away,
	// then reported "no table match" as if the walk had failed. A truncated name
	// list is now also counted and warned about, so the same silence cannot recur.
	inline constexpr std::size_t kMaxNames = 192;
	// Names found under the weapon's P-Scope attach point, de-duplicated and
	// with their ":N" shape suffix stripped. A scope is a handful of shapes,
	// so this is generous.
	inline constexpr std::size_t kMaxScopeNames = 16;
	inline constexpr std::size_t kNameLen = 64;
	// Shapes under P-Scope whose placement we record. A scope is a handful.
	inline constexpr std::size_t kMaxShapeGeom = 12;
	// Model paths. Vanilla scope paths run to ~44 characters; mods nest deeper.
	inline constexpr std::size_t kPathLen = 128;
	// Attached object mods on one weapon: receiver, barrel, stock, grip, magazine,
	// muzzle, sight, paint... Ten is a fully-built weapon; 24 is slack.
	inline constexpr std::size_t kMaxMods = 24;

	// One attached OMOD. Recorded for ALL of them, not just the one that
	// resolves, because when nothing resolves the list IS the finding: it is
	// what a user pastes into [Scopes] to teach the plugin their modded optic.
	struct ModInfo
	{
		std::uint32_t formID = 0;
		// BGSTypedKeywordValue<kAttachPoint> at Mod+0xC0 -- the slot the mod
		// occupies (scope, muzzle, ...) as a keyword INDEX, not a pointer.
		// Logged but not yet acted on: identifying the scope slot by value would
		// let us name a modded optic we have no census row for, and that wants
		// its own derivation rather than a guessed constant.
		std::uint16_t attachPoint = 0;
		char          path[kPathLen] = {};  // as authored, for humans
		char          key[kPathLen] = {};   // normalized, for lookups
	};

	// WHERE a piece of the scope is, in world space (v0.2.92).
	//
	// Radius alone cannot fit a lens -- a correctly sized disc in the wrong place
	// still misses it, which is what the 2026-08-10 screenshots showed. Placing it
	// automatically needs the scope's actual position, so the walk now records it.
	//
	// Layout is CommonLibF4's NiAVObject, spot-checked against this binary: world
	// transform +0x70, its translate +0xa0 and scale +0xac (both already used by
	// the FOV derivation since v0.2.90), worldBound +0xb0 as {centre, radius}.
	// Translate at +0x30 WITHIN the transform means NiMatrix3 is 3 groups of 4
	// floats, not 3 of 3 -- read with stride 4 or you get a shear.
	//
	// AND IT IS STORED TRANSPOSED. This is the part that cost three bugs, so it
	// is spelled out. NiTransform::operator* (0x1401a8d60) computes
	//
	//     world[j] = SUM_i v[i] * m[i*4 + j] * scale + T[j]
	//
	// so m[i*4+j] carries SOURCE axis i into DESTINATION axis j -- element
	// (row j, col i) in standard notation. CommonLibF4 says the same thing if
	// read carefully: NiMatrix3 is `NiPoint4 entry[3]` and the constructor names
	// them (x0,y0,z0,w0), (x1,y1,z1,w1)..., i.e. entry[i] is the IMAGE OF BASIS
	// VECTOR i -- a column, not a row.
	//
	//     to world:  world[j] = T[j] + s * SUM_i m[i*4 + j] * v[i]
	//     to local:  local[i] =        SUM_j m[i*4 + j] * (world[j] - T[j]) / s
	//
	// Read it the other way round and you apply the INVERSE rotation, which
	// PRESERVES LENGTH -- so every magnitude check still passes and only a
	// direction-preserving comparison catches it. ReadGeom below transposes on
	// read so that rot[] is the real rotation and callers use the textbook form.
	struct ShapeGeom
	{
		// The live node. Placement re-reads its CURRENT world transform rather
		// than using the snapshot below, so the target is same-frame with
		// everything else the placement maths touches (see OcularFaceWorld).
		std::uintptr_t node = 0;
		char  name[kNameLen] = {};
		float world[3] = {};        // world translate
		float rot[9] = {};          // world rotation, row-major, de-strided
		float scale = 0.0f;
		float boundCenter[3] = {};  // worldBound
		float boundRadius = 0.0f;
	};

	struct Info
	{
		bool          probed = false;   // a probe has completed at least once
		bool          faulted = false;  // the walk faulted; probing is disabled
		std::uint32_t weaponFormID = 0;
		float         fovMult = 1.0f;      // scope magnification (1.0 = none/unknown)
		// v0.2.111 — from the equipped weapon's BGSZoomData::Data: the reticle
		// overlay index and the imod formID. Overlay 16 = the Recon widget branch;
		// imod 0x94636 = zd_ScopeNightVision, 0x2041b6 = zd_ScopeTargetingRecon.
		// These select the lens composite's screen/NV modes now that the fullscreen
		// imods are suppressed (they were authored for a flat game where the scope
		// WAS the screen).
		std::uint32_t zoomOverlay = 0;
		std::uint32_t zoomImodID = 0;
		float         zoomFovAt90 = 90.0f;  // engine's own zoom FOV for a 90 deg base
		char          weaponNodeName[kNameLen] = {};
		// The key that resolved the table -- a model path or a node name, so it
		// is sized for the longer one. matchedBy says WHICH, because "the disc is
		// wrong" and "the disc is right for the wrong scope" look identical
		// otherwise, and the second is the whole failure mode this version fixes.
		char          matched[kPathLen] = {};
		char          matchedBy[16] = {};  // "path" | "node" | "" (nothing matched)
		// The [Scopes] key that overrode the built-in row, "" if none. Kept
		// SEPARATE from `matched` rather than overwriting it, which is what
		// v0.2.85-100 did: a user who adds an entry needs to see that it took
		// effect, and the next reader needs to see which census row is still
		// supplying the face underneath it. Conflating the two hid exactly that
		// (the v0.2.96 defect below).
		char          overrideKey[kPathLen] = {};
		float         aperture = 0.0f;   // resolved ocular radius actually in use
		// v0.2.112: height/width of a rectangular screen optic (1.0 = circular
		// optic or square screen — every row but MGScopeThermal). When < 1 the
		// aperture above is the screen's HALF-WIDTH and the lens composite masks
		// the vertical overshoot, so a wide display shows a wide picture.
		float         screenAspect = 1.0f;
		bool          fromTable = false;  // false = fell back to widgetApertureRadius
		// Per-scope widget POSITION. NaN = this scope does not specify the axis, so
		// the global widgetOffset* setting applies; 0.0 is a real, deliberate zero.
		// Radius alone cannot fit a lens: a correctly sized disc in the wrong place
		// still misses it, which is what the 2026-08-10 screenshots showed.
		float         offsetX = std::numeric_limits<float>::quiet_NaN();
		float         offsetY = std::numeric_limits<float>::quiet_NaN();
		float         offsetZ = std::numeric_limits<float>::quiet_NaN();
		std::uint32_t  nodesVisited = 0;
		std::uint32_t  nameCount = 0;
		std::uint32_t  nameOverflow = 0;  // names the buffer could not hold
		std::uint32_t  clipped = 0;       // subtrees refused by the depth/node caps
		std::uintptr_t weaponNode = 0;    // so a /read can walk the tree by hand
		char           names[kMaxNames][kNameLen] = {};
		std::uint32_t  scopeNameCount = 0;
		char           scopeNames[kMaxScopeNames][kNameLen] = {};

		// --- attached object mods (v0.2.101) --------------------------------
		// modsError is not decoration. Every step of the OMOD chain can decline
		// for an ordinary reason (a weapon with no mods has no extra data at
		// all), and without a stated reason "no mods" and "the layout moved"
		// produce the same empty list -- which is how a silently broken lookup
		// spent five versions looking like a working one (§2 of the session doc).
		bool          haveMods = false;
		std::uint32_t modCount = 0;
		std::uint32_t modOverflow = 0;
		std::uint32_t stacksSeen = 0;
		// Which rule chose the inventory stack: "instance" (its instance data IS
		// the equipped one — the answer we want) or "sole" (nothing matched but
		// there was only one stack, so it cannot be the wrong one). Recorded
		// because "sole" everywhere would mean the identity comparison never
		// works, and that is invisible from a correct-looking result.
		char          stackPick[16] = {};
		char          modsError[96] = {};
		ModInfo       mods[kMaxMods] = {};

		// --- geometry of the equipped optic (v0.2.92) ------------------------
		// Two independent measures of where the scope is, deliberately BOTH
		// recorded rather than picking one: the attach point's own worldBound
		// (which the engine maintains as a subtree union, IF it maintains it for
		// a connect point at all) and a union computed here from the descendant
		// shapes. If they disagree, that is a finding, not a coin toss.
		bool           haveGeom = false;
		std::uintptr_t pScopeNode = 0;
		ShapeGeom      pScope{};             // the attach point itself
		float          unionCenter[3] = {};  // union of descendant shape bounds
		float          unionRadius = 0.0f;
		std::uint32_t  boundsSeen = 0;       // shapes that contributed a bound
		std::uint32_t  shapeCount = 0;
		ShapeGeom      shapes[kMaxShapeGeom] = {};

		// Census-measured ocular face for this optic, if it has a row.
		bool  haveFace = false;
		char  faceShape[kNameLen] = {};  // the runtime shape to transform it by
		float face[3] = {};              // in that shape's own space
		float faceFromCentre = 0.0f;     // |face - bound centre|, for the live check
	};

	// v0.3.4 - is this object a real NiNode, i.e. safe to read its children
	// array (+0x168 base, u16 +0x172 slot count, null holes legal)? Tests the
	// vtable slot +0x188 for NiNode::GetObjectByName itself - the exact dispatch
	// the engine's own recursive find uses. Verified exact by a 2026-08-26
	// Ghidra sweep: the whole 20-class NiNode family shares the slot, nothing
	// overrides it. Reads are unguarded - call under SEH.
	[[nodiscard]] bool IsNiNode(std::uintptr_t a_obj) noexcept;

	// Ask for a probe on the next render. Cheap and idempotent; the walk itself
	// runs on the render thread because that is where the 3D is safe to touch.
	void Request();
	// v0.2.120: is a requested probe still waiting to run?
	bool ProbePending();
	// v0.2.120: completed-probe generation counter - consumers re-latch state
	// derived from ident data when this changes (the stale-bound-placement fix).
	std::uint32_t ProbeCount();
	// v0.2.121: does the matched table row promise a census face for this scope?
	// (PresenceFit retries a declined placement only while one is actually
	// expected; a heuristic-only or faulted scope latches immediately.)
	bool CensusFaceExpected();
	// v0.2.121: did the last probe actually resolve that face in the weapon's 3D?
	// (When it did, placement re-reads world transforms live, so a retry needs no
	// fresh probe; when it did not, the retry re-Requests one.)
	bool CensusFaceResolved();

	// Render-thread: run a pending probe. No-op if none was requested.
	void RunIfRequested(std::uintptr_t a_player);

	// The aperture to fit the widget with, in mesh units. Falls back to the
	// widgetApertureRadius setting when the equipped scope is not in the table,
	// so an unknown or modded scope behaves exactly as it did before v0.2.85.
	float ApertureRadius();

	// Widget offset for the equipped scope, falling back to the global
	// widgetOffset* settings for any axis this scope does not specify.
	void WidgetOffsets(float& a_x, float& a_y, float& a_z);

	// Magnification of the equipped scope (zoomData fovMult). 1.0 when unknown.
	float FovMult();

	// Where the equipped optic is, as a world-space sphere. False when the walk
	// found no P-Scope subtree or no shape carried a usable bound — in which case
	// automatic placement must decline rather than aim at the origin.
	[[nodiscard]] bool ScopeBound(float (&a_center)[3], float& a_radius);

	Info Get();

	// Built-in table, exposed so DevBench can list it. Measured from the shipped
	// meshes by tools/scope-census.py in the investigation repo (`--cpp` prints
	// exactly these rows).
	struct TableEntry
	{
		// PRIMARY key: the normalized Meshes-relative model path of the mesh
		// this row was measured from. Unique per mesh, which the node name is
		// not once mods are installed -- see the header note.
		const char* path;
		// FALLBACK key: the root node name with ":N" stripped. Still needed,
		// because the OMOD list is not always readable (a scope welded into a
		// weapon's base mesh rather than attached as a mod has no OMOD at all).
		const char* node;
		float       aperture;  // ocular radius (half-WIDTH for a rectangular screen)
		float       aspect;    // screen h/w; 1.0 = circular optic or square screen
		const char* shape;     // the ONE shape the census measured
		float       face[3];   // ocular face centre, in that shape's own space
		// |face - the shape's bounding-sphere centre|, from the mesh. Checked
		// against the LIVE bound radius: a point on a shape cannot lie outside
		// that shape's own bounding sphere, so a row failing this does not
		// describe the mesh currently in the scene.
		float faceFromCentre;
	};
	std::span<const TableEntry> Table();

	// World-space centre of the equipped optic's ocular face, taken from the
	// census and pushed through the live shape's world transform. This is the
	// exact answer where we have one; false means no census row for this scope,
	// or its measured shape is not in the scene — in which case the caller must
	// fall back to a heuristic rather than aim at nothing.
	[[nodiscard]] bool OcularFaceWorld(float (&a_world)[3]);

	// v0.2.107 — pure table lookup for one model path, exactly the compiled data
	// and normalizer the live probe uses (step-1 path pass + [Scopes] override),
	// with NO game state touched. Exists so a headless sweep can validate the
	// whole coverage matrix (DevBench /scope/lookup) without needing an OMOD
	// attached in-game — FO4VR's console AttachMod attaches to the REFERENCE, not
	// the equipped weapon, so console-driven attach is impossible on this build.
	struct PathLookup
	{
		bool  hit;          // table row or [Scopes] aperture found
		bool  fromToml;     // an override supplied/changed the aperture
		float aperture;
		float aspect;       // screen h/w (1.0 = circular/square)
		char  key[160];     // the normalized key that was looked up
		char  node[64];     // matched row's fallback node name ("" if override-only)
		char  shape[64];    // matched row's measured shape
	};
	[[nodiscard]] PathLookup LookupModelPath(const char* a_rawPath) noexcept;

	// v0.2.108 — attach (or detach) an OMOD on the player's EQUIPPED weapon, the
	// workbench-equivalent path. Exists because FO4VR's console `AttachMod`
	// attaches to the console-selected REFERENCE (the player for `player.amod`),
	// never the equipped weapon — flat FO4 fixed that in a later patch, VR never
	// did. This calls the engine's own papyrus-native backend instead:
	// GameScript::ModifyInventoryItemMod (0x141488a00) — validate, single-stack
	// check, ModifyModDataFunctor write, PostModifyInventoryItemMod re-equip.
	// The VM/stack args are ONLY dereferenced on its error paths, so the same
	// preconditions it checks are pre-checked here and nulls are safe.
	// MUST run on the game's main thread (it mutates inventory and re-equips);
	// DevBench queues it via the F4SE task interface.
	struct AttachOutcome
	{
		bool          ok;
		char          error[96];
		std::uint32_t weaponFormID;
	};
	[[nodiscard]] AttachOutcome AttachModToEquippedWeapon(std::uint32_t a_modFormID, bool a_attach) noexcept;
}
