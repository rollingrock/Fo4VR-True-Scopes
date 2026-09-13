#pragma once

namespace TrueScopes::FrikBridge
{
	// FRIK API v3 scope provider. FRIK keys body hiding, hand and recoil damping,
	// Pip-Boy interaction and two-hand grip release on one "looking through the
	// scope" state; without a provider that state is the vanilla ScopeMenu. We
	// register as the provider that keeps the body visible and publishes the
	// state ourselves, so FRIK follows the pose gate instead of the menu.
	//
	// Register on kGameLoaded (FRIK must already be loaded; the call is
	// idempotent across save loads). FRIK absent, older than the header, or
	// frikProvider=false all degrade to "no provider": one log line, nothing else
	// changes.
	void OnGameLoaded();

	// Publish the looking-through state. Game thread only, per FRIK's contract;
	// repeats are swallowed, so call it from anywhere the verdict may have moved.
	void PublishLookingThrough(bool a_looking);

	// True once setScopeProvider succeeded this session.
	bool Registered();

	// F4SE messages FRIK broadcasts under its own plugin name ("F4VRBody"):
	// kSkeletonDestroying / kSkeletonReady carry the skeleton generation and
	// mark a player-3D rebuild the engine's own messages do not always announce.
	// Registered at plugin load against that sender; works without FRIK (the
	// registration is simply never dispatched).
	void RegisterLifecycleListener();
}
