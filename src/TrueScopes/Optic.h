#pragma once

#include "TrueScopes/OpticClass.h"

namespace TrueScopes::Optic
{
	// The sight hanging under the equipped weapon's P-Scope, classified from its
	// shapes (OpticClass). Optics without HasScope never reach the verdict site, so
	// this is how an STS scope or a reflex sight is recognised at all.

	// Every frame (the fill hook). Walks when the weapon node changes and every 30
	// frames after; logs when the answer changes. Then runs the pose gate on the
	// optic's own eyepiece.
	void Tick(std::uintptr_t a_player);

	// The pose gate on the optic's eyepiece: the anchor is the ocular point,
	// P-Scope's Y the tube. Evaluated for the lens kinds only (STS magnified,
	// no-fade, far reticle) while the weapon is drawn and no menu is up, and never
	// for a HasScope weapon - the verdict site has those.
	struct Gate
	{
		bool          evaluating;
		bool          live;
		bool          sentinel;    // both eyes ahead of the eyepiece
		int           drawn;       // 1, 0, -1 = the ActorState layout did not check out
		float         dist;
		float         lateral;
		float         lookDeg;
		float         ocular[3];   // the anchor, world
		float         axis[3];     // down-range, world
		std::uint32_t evals;
		std::uint32_t edges;
		char          why[32];     // why it is not evaluating
	};

	struct Snapshot
	{
		std::uintptr_t     weapon;
		std::uintptr_t     pScope;  // 0 = no P-Scope in this weapon's 3D
		std::uint32_t      walks;
		bool               faulted;
		OpticClass::Input  in;
		OpticClass::Result result;
		std::uintptr_t     nodes[OpticClass::kMaxShapes];  // the shape behind each in.shapes[i]
		Gate               gate;
	};

	// For DevBench; any thread.
	Snapshot Get();

	// P-Scope's world transform as read this frame: rows[i] is its axis i in world.
	struct Frame
	{
		float rows[3][3];
		float t[3];
		float s;
	};

	// What the STS lens works from, this frame. Game thread only, after Tick; the
	// pointers are good until the next walk or weapon change.
	struct Lens
	{
		bool                      live;    // the gate
		std::uintptr_t            pScope;
		Frame                     frame;   // valid while live
		const OpticClass::Input*  in;
		const OpticClass::Result* result;
		const std::uintptr_t*     nodes;
	};
	Lens Current();

	// Is this P-Scope still the one under the equipped weapon? Game thread. False
	// once the weapon 3D it belonged to has been replaced: its shapes are gone.
	bool Attached(std::uintptr_t a_pScope);
}
