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

	Info Get();

	// Built-in table, exposed so DevBench can list it. Radii are measured from
	// the shipped meshes by tools/scope-census.py in the investigation repo.
	struct TableEntry
	{
		const char* node;
		float       aperture;
	};
	std::span<const TableEntry> Table();
}
