#pragma once

namespace TrueScopes::Collimator
{
	// Reflex sights at infinity through the engine's own effect shader. The glass
	// reflects its env cube with reflect(V, N), V per eye, so cube content is at
	// optical infinity for both eyes and clipped to the glass - but in world axes.
	// For the registered sight glass we rewrite the per-geometry World rows (VS
	// c8..c13, eye*3+8) into the sight's own axes, so the cube is looked up in
	// bore space and its +Y direction stays on the bore.

	// Patch the effect shader's per-geometry setup just before it unmaps the VS
	// constants. Load time, after the trampoline is allocated.
	void Install();

	// Game thread, every frame: find the equipped weapon's reflex glass and the
	// node its sight hangs under. Cheap when nothing changed.
	void Tick(std::uintptr_t a_player);
}
