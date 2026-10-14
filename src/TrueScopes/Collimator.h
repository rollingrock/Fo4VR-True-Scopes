#pragma once

namespace TrueScopes::Collimator
{
	// Reflex sights at infinity through the engine's own effect shader. The glass
	// reflects its env cube with reflect(V, N), V per eye, so cube content is at
	// optical infinity for both eyes and clipped to the glass - but in world axes.
	// For the registered sight glass we rewrite the per-geometry World rows (VS
	// c8..c13, eye*3+8) into the sight's own axes, so the cube is looked up in
	// bore space and its +Y direction stays on the bore. Vanilla panes face the
	// muzzle and the effect shader culls back faces, so each pane is also turned
	// to face the eye.

	// Patch both of the effect shader's per-geometry setups: the immediate one
	// just before it unmaps the VS constants, and the record one (vtable slot 3).
	// Load time, after the trampoline is allocated.
	void Install();

	// Every frame (the fill hook): find the equipped weapon's reflex glass and the
	// node its sight hangs under, and turn the panes. Cheap when nothing changed.
	void Tick(std::uintptr_t a_player);
}
