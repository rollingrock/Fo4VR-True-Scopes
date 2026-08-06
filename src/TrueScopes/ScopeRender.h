#pragma once

namespace TrueScopes::ScopeRender
{
	// Resolve addresses (RIP-decoded from verified code anchors) and allocate the
	// persistent BSShaderAccumulator. Returns false if any anchor fails verification —
	// the caller should then stay on the copy fill.
	bool Init();

	// Perform one mono world render from PrimaryWeaponScopeCamera into a temp render
	// target and copy it into the lens target. SEH-guarded: on any fault it logs,
	// permanently disables itself (Available() goes false), and returns false so the
	// caller can fall back to the copy fill. Must be called from the render-thread fill
	// hook (renderer locked).
	bool Render();

	// True once Init succeeded and no fault has tripped the guard.
	bool Available();
}
