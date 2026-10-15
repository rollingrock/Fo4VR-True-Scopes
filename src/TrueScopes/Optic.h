#pragma once

#include "TrueScopes/OpticClass.h"

namespace TrueScopes::Optic
{
	// The sight hanging under the equipped weapon's P-Scope, classified from its
	// shapes (OpticClass). Optics without HasScope never reach the verdict site, so
	// this is how an STS scope or a reflex sight is recognised at all.

	// Every frame (the fill hook). Walks when the weapon node changes and every 30
	// frames after; logs when the answer changes.
	void Tick(std::uintptr_t a_player);

	struct Snapshot
	{
		std::uintptr_t     weapon;
		std::uintptr_t     pScope;  // 0 = no P-Scope in this weapon's 3D
		std::uint32_t      walks;
		bool               faulted;
		OpticClass::Input  in;
		OpticClass::Result result;
	};

	// For DevBench; any thread.
	Snapshot Get();
}
