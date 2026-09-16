#pragma once

#include <cstdint>

namespace TrueScopes::Hooks
{
	// Verifies original bytes at both patch sites, then installs:
	//   1. the one-byte defang of the scope-arm setter (renderer+3 can never be set,
	//      so the vanilla scoped-frame redirect — black main view, rebuild churn,
	//      deferred-release hazards — never engages)
	//   2. the per-frame fill hook inside Main::DrawWorld_And_UI
	// Returns false (and leaves the game untouched) if any byte check fails.
	bool Install();

	// Register the unequip event sink (the teardown latch's arm signal). Once,
	// at kGameDataReady - the event source singleton exists by then.
	void RegisterEquipSink();

	// Player-3D lifecycle boundary: stands down the active scope and invalidates
	// every widget/ident result before rebuilt player 3D can draw. Reasons: F4SE
	// kGameLoaded / kPreLoadGame / kPostLoadGame / kNewGame, and FRIK's own
	// skeleton destroy/ready broadcasts - a save loaded mid-session sends only
	// the pre/post pair, and FRIK rebuilds its skeleton on it while the engine
	// hands back the same ScopeParent address, so a stale rotation calibration
	// survived until this listened to all of them. Game thread; idempotent.
	void StandDownFor(std::string_view a_reason);

	// Blocking-menu tracking for the fill cadence. Game thread writes on the
	// menu open/close edges; the fill hook reads. Returns whether a_name is one
	// of the menus the fill treats as blocking, so the caller can act on the
	// same answer (FrikBridge tells FRIK the eye is off the tube on the open
	// edge of exactly these).
	bool SetBlockingMenuOpen(std::string_view a_name, bool a_open);
	bool BlockingMenuOpen();
	void OnGameLoaded();


	// Count of per-frame fill-hook invocations, i.e. the game's frame count.
	// Advances whether or not the scope is up: ScopeRender's own `renders` counter
	// only ticks while scoped, so it cannot measure the scope-down baseline.
	// Frames/second from two reads of this is the game frame rate in any state.
	std::uint64_t FrameCount();

	// True while the scope is considered active (post-hysteresis). Lets a perf sample
	// record which state it was taken in instead of trusting the operator's notes.
	bool ScopeActive();

	// Advances by one on every ScopeActive() edge, raise and lower alike, from 0
	// with the scope down - so its parity is the state. It is the feed behind the
	// exported TrueScopes_ScopeEpisode (main.cpp): a poller comparing generations
	// sees every edge, including one that came and went between two of its frames.
	// Safe from any thread.
	std::uint64_t ScopeEpisodeGeneration();

	// Is the plugin-owned widget presence currently showing the nodes.
	bool WidgetPresenceShown();

	// Re-reads the arm-write call site and compares it to what Install() left there.
	// Call once every plugin has loaded: one that patches the same site with the old
	// f4se_common Write5Call overwrites us blind, and nothing else would ever say so.
	// Logs the mismatch. True if intact, or if Install() never ran.
	bool VerifyArmWriteHookIntact();
}
