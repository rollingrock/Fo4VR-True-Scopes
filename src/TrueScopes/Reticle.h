#pragma once

namespace TrueScopes::Reticle
{
	// Reflex and holo reticles, collimated: the sight's own reticle art is drawn
	// on its lens glass, projected by angle from each eye, so it sits on the bore
	// at infinity whichever eye looks and wherever the head is. Bruno's corrected
	// reticles from ROCK_Emitters. Hooks the shared pass draw and the cached batch
	// dispatch; any reticle that does not qualify keeps its native draw.

	// At load, only with reflexReticleEnabled. Non-fatal: a mismatched engine
	// leaves every reticle native.
	bool Install();

	// Every frame from the fill hook, after Optic::Tick. Picks the reticle cards
	// on the equipped weapon (never a magnified scope's) and publishes the aim.
	void Tick(std::uintptr_t a_player);

	// Load, stand-down: drop the published aim and every cached optic.
	void Clear();

	struct Diag
	{
		bool installed;
		const char* stage;
		std::uint64_t generation;
		std::uint32_t candidates;
		std::uint32_t excluded;  // reticle cards left alone: a magnified optic's
		char names[4][64];
		float bore[3];
		float halfAngle;
		std::uint64_t reticlePasses;
		std::uint64_t effectPasses;
		std::uint64_t draws;
		std::uint64_t packetReroutes;
		std::uint64_t techniqueRecoveries;
		std::uint64_t geometryReloads;
		std::uint64_t poseRefits;
		char lastReject[48];
		char aperture[64];  // the qualified optic's aperture shape, "" before one
	};

	// For DevBench; any thread.
	Diag GetDiag();
}
