#pragma once

// Identify the equipped scope and read how much it magnifies. The widget
// disc has to be fitted to the real scope's ocular lens and the render FOV
// to its magnification, and both are per-scope: across the shipped optics
// the ocular radius runs 0.76 - 4.56, so no single constant fits.
//
// Lookup is keyed on the attached OMOD's model path, which is unique per
// mesh. The root node name is only a fallback key: modded meshes copy a
// vanilla mesh and keep its root node name (a thermal scope wearing
// "HuntingScope" would silently get the hunting rifle's numbers), and two
// shipped scopes share a file name, so the key is the whole Meshes-relative
// path. The OMOD carries the world-model path and the engine substitutes
// "_1.nif" for ".nif" to get the first-person mesh (FUN_14130c910), so both
// spellings fold to one key -- the normalizer must stay identical to
// mesh_key() in tools/scope-census.py.
//
// Engine entry points and layout offsets are listed with their RVAs in
// ScopeIdent.cpp.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace TrueScopes::ScopeIdent
{
	// A single weapon's 3D can run past 50 nodes, so this is sized well past a
	// full weapon; a truncated list is counted (nameOverflow) and warned about.
	inline constexpr std::size_t kMaxNames = 192;
	// Names found under the weapon's P-Scope attach point, de-duplicated and
	// with their ":N" shape suffix stripped. A scope is a handful of shapes,
	// so this is generous.
	inline constexpr std::size_t kMaxScopeNames = 16;
	inline constexpr std::size_t kNameLen = 64;
	// Shapes under P-Scope whose placement is recorded. A scope is a handful.
	inline constexpr std::size_t kMaxShapeGeom = 12;
	// Model paths. Vanilla scope paths run to ~44 characters; mods nest deeper.
	inline constexpr std::size_t kPathLen = 128;
	// Attached object mods on one weapon: receiver, barrel, stock, grip, magazine,
	// muzzle, sight, paint... Ten is a fully-built weapon; 24 is slack.
	inline constexpr std::size_t kMaxMods = 24;

	// One attached OMOD. All of them are recorded, not just the one that
	// resolves: when nothing resolves, the list is what a user pastes into
	// [Scopes] to teach the plugin their modded optic.
	struct ModInfo
	{
		std::uint32_t formID = 0;
		// BGSTypedKeywordValue<kAttachPoint> at Mod+0xC0 -- the slot the mod
		// occupies (scope, muzzle, ...) as a keyword index, not a pointer.
		// Logged but not acted on yet.
		std::uint16_t attachPoint = 0;
		char          path[kPathLen] = {};  // as authored, for humans
		char          key[kPathLen] = {};   // normalized, for lookups
	};

	// Where a piece of the scope is, in world space. NiAVObject layout,
	// spot-checked against this binary: world transform +0x70, translate +0xa0,
	// scale +0xac, worldBound +0xb0 as {centre, radius}. Translate at +0x30
	// within the transform means NiMatrix3 is 3 groups of 4 floats, not 3 of 3
	// -- read with stride 4 or you get a shear.
	//
	// The matrix is stored transposed: NiTransform::operator* (0x1401a8d60)
	// computes world[j] = T[j] + s * SUM_i m[i*4 + j] * v[i], i.e. entry[i] is
	// the image of basis vector i -- a column, not a row. ReadGeom transposes
	// on read so rot[] is the real rotation and callers use the textbook form.
	// A rotation applied the wrong way round preserves length, so magnitude
	// checks do not catch that mistake; only a direction comparison does.
	struct ShapeGeom
	{
		// The live node. Placement re-reads its current world transform rather
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
		// From the equipped weapon's BGSZoomData::Data: the reticle overlay index
		// and the imod formID. Overlay 16 = the Recon widget branch; imod 0x94636
		// = zd_ScopeNightVision, 0x2041b6 = zd_ScopeTargetingRecon. These select
		// the lens composite's screen/NV modes (the fullscreen imods, authored
		// for the flat game, are suppressed).
		std::uint32_t zoomOverlay = 0;
		std::uint32_t zoomImodID = 0;
		float         zoomFovAt90 = 90.0f;  // engine's own zoom FOV for a 90 deg base
		char          weaponNodeName[kNameLen] = {};
		// The key that resolved the table -- a model path or a node name, so it
		// is sized for the longer one. matchedBy records which, because "the disc
		// is wrong" and "the disc is right for the wrong scope" look identical
		// otherwise.
		char          matched[kPathLen] = {};
		char          matchedBy[16] = {};  // "path" | "node" | "" (nothing matched)
		// The [Scopes] key that overrode the built-in row, "" if none. Kept
		// separate from `matched`: a user who adds an entry needs to see that it
		// took effect, and the log needs to show which census row is still
		// supplying the face underneath it.
		char          overrideKey[kPathLen] = {};
		float         aperture = 0.0f;   // resolved ocular radius actually in use
		// Height/width of a rectangular screen optic (1.0 = circular optic or
		// square screen — every row but MGScopeThermal). When < 1 the aperture
		// above is the screen's half-width and the lens composite masks the
		// vertical overshoot, so a wide display shows a wide picture.
		float         screenAspect = 1.0f;
		bool          fromTable = false;  // false = fell back to widgetApertureRadius
		// Eye relief the optic was built for, game units (0 = not specified,
		// use eyeBoxReliefUnits). Long-eye-relief pistol scopes carry a
		// built-in value; [Scopes] eyeRelief overrides.
		float         eyeRelief = 0.0f;
		// Per-scope widget position. NaN = this scope does not specify the axis,
		// so the global widgetOffset* setting applies; 0.0 is a real, deliberate
		// zero.
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

		// attached object mods
		// Every step of the OMOD chain can decline for an ordinary reason (a
		// weapon with no mods has no extra data at all), so modsError states the
		// reason: "no mods" and "the layout moved" must not produce the same
		// empty list.
		bool          haveMods = false;
		std::uint32_t modCount = 0;
		std::uint32_t modOverflow = 0;
		std::uint32_t stacksSeen = 0;
		// Which rule chose the inventory stack: "instance" (its instance data is
		// the equipped one) or "sole" (nothing matched but there was only one
		// stack, so it cannot be the wrong one). Recorded because "sole"
		// everywhere would mean the identity comparison never works, which is
		// invisible from a correct-looking result.
		char          stackPick[16] = {};
		char          modsError[96] = {};
		ModInfo       mods[kMaxMods] = {};

		// geometry of the equipped optic
		// Two independent measures of where the scope is, both recorded: the
		// attach point's own worldBound (which the engine may or may not maintain
		// for a connect point) and a union computed here from the descendant
		// shapes. If they disagree, that is a finding.
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

	// Is this object a real NiNode, i.e. safe to read its children array
	// (+0x168 base, u16 +0x172 slot count, null holes legal)? Tests vtable slot
	// +0x188 for NiNode::GetObjectByName itself - the exact dispatch the
	// engine's own recursive find uses; the whole 20-class NiNode family shares
	// the slot and nothing overrides it. Reads are unguarded - call under SEH.
	[[nodiscard]] bool IsNiNode(std::uintptr_t a_obj) noexcept;

	// Ask for a probe. Cheap and idempotent; the walk itself runs from the
	// game-thread verdict site (see RunIfRequested).
	void Request();
	// Is a requested probe still waiting to run?
	bool ProbePending();
	// Completed-probe generation counter - consumers re-latch state derived
	// from ident data when this changes.
	std::uint32_t ProbeCount();
	// Does the matched table row promise a census face for this scope?
	// (PresenceFit retries a declined placement only while one is actually
	// expected; a heuristic-only or faulted scope latches immediately.)
	bool CensusFaceExpected();
	// Did the last probe actually resolve that face in the weapon's 3D? When it
	// did, placement re-reads world transforms live, so a retry needs no fresh
	// probe; when it did not, the retry re-Requests one.
	bool CensusFaceResolved();

	// Run a pending probe. No-op if none was requested. Game thread (the verdict
	// call-site thunk) - the thread that owns the weapon 3D and the inventory,
	// so the walk and the extra-data chain never race a teardown. The fill hook
	// keeps a fallback call for the no-weapon-drawn DevBench case, gated on the
	// verdict site being long dead.
	void RunIfRequested(std::uintptr_t a_player);

	// FormID of the weapon the last probe identified (0 = none / faulted). The
	// unequip event sink filters on it.
	std::uint32_t CurrentWeaponFormID();

	// Drop every cached scene pointer (weapon node, shape nodes, census face,
	// bounds) without touching the resolved aperture/offsets. Called from the
	// unequip event: the pointers are about to dangle, and a name that still
	// reads back correctly from freed-but-intact memory would pass the live
	// re-checks. The next probe repopulates everything.
	void InvalidateNodes();

	// Clear the entire resolved answer and request a fresh probe. Weapon changes
	// and save-load rebuilds must not retain the previous optic's aperture,
	// placement, FOV or scene pointers while the replacement 3D is settling.
	void InvalidateForLifecycle();

	// The aperture to fit the widget with, in mesh units. Falls back to the
	// widgetApertureRadius setting when the equipped scope is not in the table.
	float ApertureRadius();

	// Widget offset for the equipped scope, falling back to the global
	// widgetOffset* settings for any axis this scope does not specify.
	void WidgetOffsets(float& a_x, float& a_y, float& a_z);

	// Magnification of the equipped scope (zoomData fovMult). 1.0 when unknown.
	float FovMult();

	// Eye relief of the equipped optic in game units, 0 when it does not
	// specify one (the caller falls back to eyeBoxReliefUnits). Long-eye-relief
	// pistol scopes carry a built-in value; [Scopes] eyeRelief overrides.
	float EyeReliefUnits();

	// Where the equipped optic is, as a world-space sphere. False when the walk
	// found no P-Scope subtree or no shape carried a usable bound — in which case
	// automatic placement must decline rather than aim at the origin.
	[[nodiscard]] bool ScopeBound(float (&a_center)[3], float& a_radius);

	Info Get();

	// Built-in table, exposed so DevBench can list it. Measured from the shipped
	// meshes by tools/scope-census.py (`--cpp` prints exactly these rows).
	struct TableEntry
	{
		// Primary key: the normalized Meshes-relative model path of the mesh
		// this row was measured from. Unique per mesh, which the node name is
		// not once mods are installed.
		const char* path;
		// Fallback key: the root node name with ":N" stripped. Still needed,
		// because the OMOD list is not always readable (a scope welded into a
		// weapon's base mesh rather than attached as a mod has no OMOD at all).
		const char* node;
		float       aperture;  // ocular radius (half-width for a rectangular screen)
		float       aspect;    // screen h/w; 1.0 = circular optic or square screen
		const char* shape;     // the one shape the census measured
		float       face[3];   // ocular face centre, in that shape's own space
		// |face - the shape's bounding-sphere centre|, from the mesh. Checked
		// against the live bound radius: a point on a shape cannot lie outside
		// that shape's own bounding sphere, so a row failing this does not
		// describe the mesh currently in the scene.
		float faceFromCentre;
	};
	std::span<const TableEntry> Table();

	// World-space centre of the equipped optic's ocular face, taken from the
	// census and pushed through the live shape's world transform. This is the
	// exact answer where there is one; false means no census row for this scope,
	// or its measured shape is not in the scene — in which case the caller must
	// fall back to a heuristic rather than aim at nothing.
	[[nodiscard]] bool OcularFaceWorld(float (&a_world)[3]);

	// Live world rotation of the census face shape (row-major, de-strided true
	// rotation - the weapon's current orientation). Same SEH-guarded name
	// re-check as OcularFaceWorld; false when there is no resolved face or the
	// node is gone. Feeds the widget's rotation tracking.
	[[nodiscard]] bool OcularShapeRotation(float (&a_rot)[9]);

	// Pure table lookup for one model path, exactly the compiled data and
	// normalizer the live probe uses (path pass + [Scopes] override), with no
	// game state touched. Lets a headless sweep validate the whole coverage
	// matrix (DevBench /scope/lookup) without needing an OMOD attached in-game
	// — FO4VR's console AttachMod attaches to the reference, not the equipped
	// weapon, so console-driven attach is impossible on this build.
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

	// Attach (or detach) an OMOD on the player's equipped weapon, the
	// workbench-equivalent path. FO4VR's console `AttachMod` attaches to the
	// console-selected reference, never the equipped weapon, so this calls the
	// engine's own papyrus-native backend instead:
	// GameScript::ModifyInventoryItemMod (0x141488a00) — validate, single-stack
	// check, ModifyModDataFunctor write, PostModifyInventoryItemMod re-equip.
	// The VM/stack args are only dereferenced on its error paths, so the same
	// preconditions it checks are pre-checked here and nulls are safe.
	// Must run on the game's main thread (it mutates inventory and re-equips);
	// DevBench queues it via the F4SE task interface.
	struct AttachOutcome
	{
		bool          ok;
		char          error[96];
		std::uint32_t weaponFormID;
	};
	[[nodiscard]] AttachOutcome AttachModToEquippedWeapon(std::uint32_t a_modFormID, bool a_attach) noexcept;
}
