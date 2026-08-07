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

	// True only while our own deferred-resolve call is on the stack. The two
	// resolve accum-bind call-site hooks (Hooks.cpp) key off this to force
	// bind mode 3 (no clear) so our pre-drawn sun pass survives into the composite.
	bool InOwnResolve();

	// Hooks::Install reports whether the two resolve bind sites were hooked; the sun
	// pre-draw is skipped when they are not (it would just be cleared again).
	void SetSunBindHooksInstalled(bool a_installed);
}
