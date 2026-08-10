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
	inline constexpr std::size_t kMaxNames = 40;
	inline constexpr std::size_t kNameLen = 64;

	struct Info
	{
		bool          probed = false;   // a probe has completed at least once
		bool          faulted = false;  // the walk faulted; probing is disabled
		std::uint32_t weaponFormID = 0;
		float         fovMult = 1.0f;      // scope magnification (1.0 = none/unknown)
		float         zoomFovAt90 = 90.0f;  // engine's own zoom FOV for a 90 deg base
		char          weaponNode[kNameLen] = {};
		char          matched[kNameLen] = {};  // node name that keyed the table ("" = none)
		float         aperture = 0.0f;         // resolved ocular radius actually in use
		bool          fromTable = false;       // false = fell back to widgetApertureRadius
		std::uint32_t nodesVisited = 0;
		std::uint32_t nameCount = 0;
		char          names[kMaxNames][kNameLen] = {};
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
