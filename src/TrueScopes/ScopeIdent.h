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
		float         zoomFovAt90 = 90.0f;  // engine's own zoom FOV for a 90 deg base
		char          weaponNodeName[kNameLen] = {};
		char          matched[kNameLen] = {};  // node name that keyed the table ("" = none)
		float         aperture = 0.0f;         // resolved ocular radius actually in use
		bool          fromTable = false;       // false = fell back to widgetApertureRadius
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

	// Ask for a probe on the next render. Cheap and idempotent; the walk itself
	// runs on the render thread because that is where the 3D is safe to touch.
	void Request();

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
		const char* node;      // runtime key: the root name with ":N" stripped
		float       aperture;  // ocular radius
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
}
